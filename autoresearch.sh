#!/usr/bin/env bash
#
# Benchmark entrypoint for the rendering-pipeline autoresearch loop.
#
# Builds lotus_render_bench (Release) and renders one fixed synthetic scene
# (bench/render_bench.cpp: ~950 entities, 190k vertices drawn, 17 textured
# batches, shadows + SSAO + FXAA + sky + fog + Hi-Z occlusion) through the
# offscreen pipeline, up to three times:
#
#   1. On the system's default Vulkan device: the number a user would see. On
#      a discrete GPU this workload is CPU-bound (scene build + upload), so
#      the per-phase metrics matter.
#   2. On Mesa lavapipe, a software rasterizer that is bit-exact from run to
#      run. It carries the fidelity check, and its GPU time is a stable,
#      heavily amplified signal for GPU-side work (passes, shaders, overdraw).
#   3. On an integrated GPU, when the machine has one that is not the system
#      device (an Intel Vulkan ICD is looked for; override with
#      LOTUS_BENCH_IGPU_ICD). A discrete card maps cached readback memory, an
#      integrated one can map write-combined memory where ordinary loads are
#      uncached: the Hi-Z readback alone cost 0.85 ms of a 1.4 ms CPU frame
#      there, which the system device cannot show. Skipped, quietly, where
#      there is none; no igpu_* metric is then reported.
#
# Every frame is synchronous (record, submit, wait for idle): occlusion culling
# reads the previous frame's Hi-Z only once its fence has signalled, so an
# unsynchronised loop would draw a timing-dependent set of entities.
#
# METRIC lines (milliseconds, lower is better, unless noted):
#
#   frame_ms          PRIMARY. Median synchronous frame on the system device.
#                     Every system-device metric below is the median over 3
#                     processes of the per-process median (3 x 120 frames).
#   cpu_ms            ... CPU side of it (begin + build + record + submit)
#   gpu_ms            ... time waiting for the GPU after submit
#   build_ms          Scene::upload(): cull, LOD, vertex transform, upload copy
#   begin_ms          begin_frame_offscreen(): fence poll, Hi-Z unpack
#   record_ms         shadow/lit/FXAA/SSAO/Hi-Z pass recording
#   submit_ms         end_frame() submit
#   frame_ms_p90      p90 frame time (tail)
#   vertices          vertices drawn in the fidelity frame (workload counter)
#   draw_calls        draw batches in the fidelity frame (workload counter)
#   drawn_entities    entities drawn in the fidelity frame (workload counter)
#   image_mae         mean abs error (0-255) of the system device's fidelity
#                     frame vs the golden. Not zero by design: the golden is a
#                     lavapipe image, other rasterizers differ by a few levels.
#                     Compare against the baseline value; a jump means the
#                     picture changed.
#   image_max_diff    worst per-channel error of the same comparison
#   lvp_frame_ms      lavapipe frame median
#   lvp_gpu_ms        lavapipe GPU wait (software rasterization time)
#   lvp_cpu_ms        lavapipe CPU side
#   lvp_image_mae     lavapipe vs golden (0 = pixel-identical to the baseline)
#   lvp_image_max_diff
#   lvp_image_exact   1 when the full-resolution lavapipe image hash matches
#                     the golden's (higher is better; only reported while the
#                     golden's device name matches)
#   igpu_frame_ms     integrated GPU: frame median (median of 3 processes)
#   igpu_cpu_ms       ... CPU side (the metric that exposed the readback cost)
#   igpu_gpu_ms       ... time waiting for the GPU after submit
#   igpu_begin_ms     ... begin_frame_offscreen(): fence poll, Hi-Z unpack
#   igpu_build_ms     ... Scene::upload()
#   igpu_record_ms    ... pass recording
#   igpu_image_mae    ... fidelity frame vs the golden (not zero by design)
#
# Noise (this machine, pinned to the P-cores): frame_ms CV ~0.8% between
# invocations, lvp_* CV 0.3-0.4%, so wins of ~2% or more on frame_ms are
# visible. The machine also drifts by up to ~5% over tens of minutes (boost
# clocks, desktop load): re-measure the baseline before trusting a smaller
# win, and use lvp_gpu_ms (tight) for GPU-side work.
#
# Environment:
#   LOTUS_BUILD_DIR     CMake build dir (default: build)
#   LOTUS_BENCH_ICD     Vulkan ICD json for the primary run (default: the
#                       loader's own choice, i.e. the discrete GPU)
#   LOTUS_BENCH_LVP_ICD lavapipe ICD json (default: searched for)
#   LOTUS_BENCH_IGPU_ICD Vulkan ICD json of an integrated GPU to also benchmark
#                       (default: an Intel ICD if present and not the system
#                       device)
#   LOTUS_BENCH_CPUS    CPU list to pin to, or "none" (default: the fastest
#                       cores; see fast_cpus)
#   GLSLANG             glslangValidator used to keep shader headers in sync
#
# Regenerate the golden after an intentional visual change (lavapipe only):
#   VK_DRIVER_FILES=<lvp icd> LP_NUM_THREADS=8 build/bin/lotus_render_bench \
#       --assets-dir build/bench_assets --write-golden bench/golden/render_bench.png
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

build_dir="${LOTUS_BUILD_DIR:-build}"
bench="$build_dir/bin/lotus_render_bench"
golden="bench/golden/render_bench.png"

log() { echo "autoresearch: $*" >&2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- shaders -----------------------------------------------------------------
# The SPIR-V headers are committed and are what the renderer actually loads. A
# GLSL edit whose header was never regenerated would be benchmarked as a no-op,
# so when a shader source is newer than its header, rebuild the headers into a
# scratch dir and copy over only those whose bytes differ (rewriting identical
# bytes would bump mtimes and force a pointless renderer.cpp recompile).
find_glslang() {
	local candidate
	for candidate in "${GLSLANG:-}" "$(command -v glslangValidator || true)" \
	                 /tmp/shadertools/usr/bin/glslangValidator; do
		if [[ -n "$candidate" && -x "$candidate" ]]; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}

sync_shaders() {
	local src hdr stale=0
	for src in shaders/*.vert shaders/*.frag shaders/*.comp; do
		hdr="include/lotus/gpu/$(basename "$src").spv.h"
		if [[ -f "$hdr" && "$src" -nt "$hdr" ]]; then
			stale=1
		fi
	done
	if (( stale == 0 )); then
		return 0
	fi
	local compiler
	if ! compiler="$(find_glslang)"; then
		log "WARNING: a shader source is newer than its SPIR-V header but no" \
		    "glslangValidator was found (set GLSLANG=...; see" \
		    "shaders/build-shaders.sh). The benchmark may be measuring a stale shader."
		return 0
	fi
	mkdir -p "$work/shaders"
	if ! OUT_DIR="$work/shaders" GLSLANG="$compiler" shaders/build-shaders.sh \
	        >"$work/shaders.log" 2>&1; then
		cat "$work/shaders.log" >&2
		log "shader compilation failed"
		return 1
	fi
	local header dest
	for header in "$work"/shaders/*.spv.h; do
		dest="include/lotus/gpu/$(basename "$header")"
		if ! cmp -s "$header" "$dest"; then
			cp "$header" "$dest"
			log "regenerated $dest from its GLSL source"
		fi
	done
	# Every header now matches its source. Say so with the mtime, or the test
	# at the top fires (and glslang runs) on every later invocation: a git
	# checkout that discards an experiment can leave a source a few
	# milliseconds newer than its identical header.
	for src in shaders/*.vert shaders/*.frag shaders/*.comp; do
		hdr="include/lotus/gpu/$(basename "$src").spv.h"
		if [[ -f "$hdr" && "$src" -nt "$hdr" ]]; then
			touch "$hdr"
		fi
	done
}

# --- build -------------------------------------------------------------------
build() {
	if ! { cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release &&
	       cmake --build "$build_dir" --target lotus_render_bench \
	             -j"$(nproc)"; } >"$work/build.log" 2>&1; then
		tail -n 80 "$work/build.log" >&2
		log "build failed"
		return 1
	fi
}

# --- placement ---------------------------------------------------------------
# CPUs within 10% of the fastest core's max frequency. On a hybrid CPU that is
# the P-cores: the scheduler otherwise migrates the (single-threaded, ~7 ms of
# compute per frame) benchmark between P and E cores, which was the dominant
# noise source measured on this class of machine (sigma 2.3% -> 0.7% pinned).
# On a homogeneous CPU it is every core, so pinning is a no-op. Prints nothing
# when frequencies are unavailable (containers, VMs).
fast_cpus() {
	command -v lscpu >/dev/null 2>&1 || return 0
	LC_ALL=C lscpu -p=CPU,MAXMHZ 2>/dev/null | awk -F, '
		/^#/ { next }
		{ n++; cpu[n] = $1; mhz[n] = $2 + 0; if (mhz[n] > max) max = mhz[n] }
		END {
			if (max == 0) exit
			sep = ""
			for (i = 1; i <= n; i++) {
				if (mhz[i] >= 0.9 * max) { printf "%s%s", sep, cpu[i]; sep = "," }
			}
		}'
}

cpus="${LOTUS_BENCH_CPUS:-$(fast_cpus)}"
pin=()
if [[ -n "$cpus" && "$cpus" != "none" ]] && command -v taskset >/dev/null 2>&1 &&
   taskset -c "$cpus" true 2>/dev/null; then
	pin=(taskset -c "$cpus")
	cpu_count="$(taskset -c "$cpus" nproc)"
else
	cpus="none"
	cpu_count="$(nproc)"
fi
# Lavapipe worker threads: one per pinned core, at most 8 (more threads did not
# make the output differ, only the machine busier).
lp_threads=$(( cpu_count < 8 ? cpu_count : 8 ))

find_lvp_icd() {
	local candidate
	for candidate in "${LOTUS_BENCH_LVP_ICD:-}" \
	                 /usr/share/vulkan/icd.d/lvp_icd*.json \
	                 /etc/vulkan/icd.d/lvp_icd*.json \
	                 /usr/local/share/vulkan/icd.d/lvp_icd*.json; do
		if [[ -n "$candidate" && -f "$candidate" ]]; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}

run_bench() { # run_bench <output file> [env VAR=value ...] -- [bench args ...]
	local out="$1"
	shift
	local env_args=()
	while [[ $# -gt 0 && "$1" != "--" ]]; do
		env_args+=("$1")
		shift
	done
	shift # the --
	if ! env ${env_args[@]+"${env_args[@]}"} ${pin[@]+"${pin[@]}"} "$bench" \
	        --assets-dir "$build_dir/bench_assets" --golden "$golden" "$@" \
	        >"$out" 2>&1; then
		cat "$out" >&2
		log "lotus_render_bench failed"
		return 1
	fi
}

sync_shaders
build

lvp_icd="$(find_lvp_icd)" || {
	log "no lavapipe ICD found (install mesa-vulkan-drivers, or set LOTUS_BENCH_LVP_ICD):" \
	    "the bit-exact fidelity check needs it"
	exit 1
}

# 1. System device (primary). Run first: it is the CPU-bound, noise-sensitive
# one. Several processes, reporting each metric's median: within one process
# the frame distribution moves as a whole (median, mean and p10 all had the
# same ~1.1% run-to-run CV), so only independent processes average that out.
sys_env=()
if [[ -n "${LOTUS_BENCH_ICD:-}" ]]; then
	sys_env=(VK_DRIVER_FILES="$LOTUS_BENCH_ICD" VK_ICD_FILENAMES="$LOTUS_BENCH_ICD")
fi
sys_runs=3 # odd, so the median is a measured value, not an average
for i in $(seq 1 "$sys_runs"); do
	run_bench "$work/system.$i.out" ${sys_env[@]+"${sys_env[@]}"} --
done

# 2. Lavapipe: fewer frames, it is ~6x slower per frame, and its CV is ~0.3%.
run_bench "$work/lvp.out" VK_DRIVER_FILES="$lvp_icd" VK_ICD_FILENAMES="$lvp_icd" \
          LP_NUM_THREADS="$lp_threads" -- --frames 30 --rounds 2 --warmup 10

# 3. An integrated GPU, when the machine has one that is not the system device.
# See the header: the system device cannot show the costs of write-combined
# readback memory. Skipped, quietly, where there is none.
find_igpu_icd() {
	local candidate
	for candidate in "${LOTUS_BENCH_IGPU_ICD:-}" \
	                 /usr/share/vulkan/icd.d/intel_icd*.json \
	                 /etc/vulkan/icd.d/intel_icd*.json; do
		if [[ -n "$candidate" && -f "$candidate" ]]; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}
device_of() { sed -n -E 's/^bench: device="([^"]*)".*/\1/p' "$1" | head -n 1; }

igpu_runs=0
igpu_device=""
igpu_icd="$(find_igpu_icd || true)"
if [[ -n "$igpu_icd" ]]; then
	for i in 1 2 3; do
		if run_bench "$work/igpu.$i.out" VK_DRIVER_FILES="$igpu_icd" \
		             VK_ICD_FILENAMES="$igpu_icd" -- 2>/dev/null; then
			igpu_runs=$((igpu_runs + 1))
		else
			break
		fi
	done
	igpu_device="$(device_of "$work/igpu.1.out" 2>/dev/null || true)"
	if (( igpu_runs != 3 )) || [[ -z "$igpu_device" ]] ||
	   [[ "$igpu_device" == "$(device_of "$work/system.1.out")" ]]; then
		igpu_runs=0 # no such hardware, or it is the system device already
	fi
fi

# Median of each METRIC across the system runs (keeps the measured value's own
# formatting, so integer counters stay integers).
median_metrics() {
	awk '
		function median(list,    a, n, i, j, t) {
			n = split(list, a, " ")
			for (i = 2; i <= n; i++) {
				t = a[i]
				for (j = i - 1; j >= 1 && a[j] + 0 > t + 0; j--) a[j + 1] = a[j]
				a[j + 1] = t
			}
			return a[int((n + 1) / 2)]
		}
		/^METRIC / {
			split($2, kv, "=")
			if (!(kv[1] in seen)) { seen[kv[1]] = 1; order[++count] = kv[1] }
			values[kv[1]] = values[kv[1]] " " kv[2]
		}
		END { for (i = 1; i <= count; i++) print "METRIC " order[i] "=" median(values[order[i]]) }
	' "$@"
}

log "cpus=$cpus lavapipe_threads=$lp_threads system_runs=$sys_runs build_dir=$build_dir"
echo "=== system device (run 1 of $sys_runs) ==="
grep -v -E '^(METRIC|ASI) ' "$work/system.1.out" || true
echo "system frame_ms per run:" \
     "$(grep -h '^METRIC frame_ms=' "$work"/system.*.out | cut -d= -f2 | tr '\n' ' ')"
echo "=== lavapipe ==="
grep -v -E '^(METRIC|ASI) ' "$work/lvp.out" || true

echo "=== results ==="
median_metrics "$work"/system.*.out
grep -E '^ASI ' "$work/system.1.out"
sed -n -E 's/^METRIC (frame_ms|gpu_ms|cpu_ms|image_mae|image_max_diff|image_exact)=/METRIC lvp_\1=/p' \
    "$work/lvp.out"
sed -n 's/^ASI /ASI lvp_/p' "$work/lvp.out"

if (( igpu_runs == 3 )); then
	echo "=== integrated GPU: $igpu_device (run 1 of 3) ==="
	grep -v -E '^(METRIC|ASI) ' "$work/igpu.1.out" || true
	median_metrics "$work"/igpu.*.out |
	    sed -n -E 's/^METRIC (frame_ms|cpu_ms|gpu_ms|begin_ms|build_ms|record_ms|image_mae)=/METRIC igpu_\1=/p'
else
	log "no integrated GPU benchmarked (none found, or it is the system device)"
fi
