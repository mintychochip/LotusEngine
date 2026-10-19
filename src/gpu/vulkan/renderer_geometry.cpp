// Mesh cache, per-frame streams, uploads and draw recording.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include <cstring>
#include <stdexcept>

namespace lotus {
namespace {

constexpr uint32_t kReservedInstances = 1;

uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void GpuRendererVulkan::grow_streams(FrameSlot& slot, uint32_t vertices,
                                     uint32_t instances)
{
	const uint64_t vertex_bytes = static_cast<uint64_t>(vertices) * sizeof(GpuVertex);
	const uint64_t uv_bytes = static_cast<uint64_t>(vertices) * sizeof(glm::vec2);
	const uint64_t instance_bytes =
	        static_cast<uint64_t>(instances + kReservedInstances) * sizeof(GpuInstance);
	if (vertex_bytes > slot.immediate_capacity || uv_bytes > slot.immediate_uv_capacity) {
		uint64_t next    = slot.immediate_capacity ? slot.immediate_capacity : 128 * 1024;
		uint64_t uv_next = slot.immediate_uv_capacity ? slot.immediate_uv_capacity
		                                              : 128 * 1024 / 2u;
		while (next < vertex_bytes) {
			next *= 2;
		}
		while (uv_next < uv_bytes) {
			uv_next *= 2;
		}
		vk::Buffer old_immediate    = slot.immediate;
		vk::Buffer old_immediate_uv = slot.immediate_uv;
		slot.garbage.push_back([this, old_immediate, old_immediate_uv]() mutable {
			memory_.destroy_buffer(old_immediate);
			memory_.destroy_buffer(old_immediate_uv);
		});
		slot.immediate = memory_.create_buffer(vk::MemoryClass::Dynamic,
		                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                                       next, "immediate");
		slot.immediate_uv = memory_.create_buffer(vk::MemoryClass::Dynamic,
		                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                                          uv_next, "immediate_uv");
		slot.immediate_capacity    = static_cast<uint32_t>(next);
		slot.immediate_uv_capacity = static_cast<uint32_t>(uv_next);
		slot.immediate_state       = vk::BufferState{};
		slot.immediate_uv_state    = vk::BufferState{};
		vertex_capacity_ = std::max(vertex_capacity_, slot.immediate_capacity);
	}
	if (instance_bytes > slot.instance_capacity * sizeof(GpuInstance)) {
		uint64_t next = slot.instance_capacity
		                        ? static_cast<uint64_t>(slot.instance_capacity) *
		                                  sizeof(GpuInstance)
		                        : 1024 * sizeof(GpuInstance);
		while (next < instance_bytes) {
			next *= 2;
		}
		vk::Buffer old = slot.instances;
		slot.garbage.push_back([this, old]() mutable {
			memory_.destroy_buffer(old);
		});
		slot.instances = memory_.create_buffer(vk::MemoryClass::Dynamic,
		                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                                       next, "instances");
		slot.instance_capacity =
		        static_cast<uint32_t>(next / sizeof(GpuInstance));
		slot.instance_state = vk::BufferState{};
	}
}

void GpuRendererVulkan::grow_uniforms(FrameSlot& slot, uint64_t size)
{
	if (slot.uniform_offset + size <= slot.uniform_capacity) {
		return;
	}
	uint64_t next = slot.uniform_capacity ? slot.uniform_capacity : 64 * 1024;
	while (next < slot.uniform_offset + size) {
		next *= 2;
	}
	vk::Buffer old = slot.uniforms;
	slot.garbage.push_back([this, old]() mutable {
		memory_.destroy_buffer(old);
	});
	slot.uniforms = memory_.create_buffer(vk::MemoryClass::Dynamic,
	                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                                      next, "uniforms");
	slot.uniform_capacity = next;
	slot.uniform_state    = vk::BufferState{};
}

uint64_t GpuRendererVulkan::push_uniforms(const void* data, uint64_t size)
{
	FrameSlot& slot = *slot_;
	grow_uniforms(slot, size);
	const uint64_t offset =
	        align_up(slot.uniform_offset, uniform_alignment_);
	if (offset + size > slot.uniform_capacity) {
		grow_uniforms(slot, offset + size - slot.uniform_offset);
	}
	std::memcpy(static_cast<uint8_t*>(slot.uniforms.mapped) + offset, data, size);
	slot.uniform_offset = offset + size;
	memory_.flush(slot.uniforms.allocation, offset, size);
	return offset;
}

bool GpuRendererVulkan::mesh_live(GpuMeshId mesh) const
{
	return mesh < meshes_.size() && meshes_[mesh].live;
}

uint32_t GpuRendererVulkan::mesh_index_count(GpuMeshId mesh) const
{
	return mesh_live(mesh) ? meshes_[mesh].index_count : 0u;
}

uint32_t GpuRendererVulkan::cached_mesh_count() const
{
	return static_cast<uint32_t>(mesh_ids_.size());
}

void GpuRendererVulkan::release_mesh(GpuMeshId mesh)
{
	if (!mesh_live(mesh)) {
		return;
	}
	CachedMesh& cached = meshes_[mesh];
	// To the current slot's garbage: the GPU may still be reading the mesh
	// from the frame in flight, whose fence has not been waited on.
	FrameSlot* target = slot_ != nullptr ? slot_ : &slots_[slot_index_];
	vk::Buffer vertex = cached.vertex;
	vk::Buffer uv     = cached.uv;
	vk::Buffer index  = cached.index;
	target->garbage.push_back([this, vertex, uv, index]() mutable {
		memory_.destroy_buffer(vertex);
		memory_.destroy_buffer(uv);
		memory_.destroy_buffer(index);
	});
	mesh_ids_.erase(cached.key);
	std::erase_if(pending_meshes_, [mesh](const PendingMeshUpload& pending) {
		return pending.id == mesh;
	});
	cached = CachedMesh{};
	free_mesh_ids_.push_back(mesh);
}

void GpuRendererVulkan::evict_idle_meshes()
{
	for (GpuMeshId id = 0; id < meshes_.size(); ++id) {
		const CachedMesh& cached = meshes_[id];
		if (cached.live && frames_rendered_ - cached.last_used > kMeshIdleFrames) {
			release_mesh(id);
		}
	}
}

void GpuRendererVulkan::destroy_meshes()
{
	for (GpuMeshId id = 0; id < meshes_.size(); ++id) {
		if (meshes_[id].live) {
			vk::Buffer vertex = meshes_[id].vertex;
			vk::Buffer uv     = meshes_[id].uv;
			vk::Buffer index  = meshes_[id].index;
			// shutdown() and the slot teardown both wait idle first.
			memory_.destroy_buffer(vertex);
			memory_.destroy_buffer(uv);
			memory_.destroy_buffer(index);
		}
	}
	meshes_.clear();
	free_mesh_ids_.clear();
	mesh_ids_.clear();
	pending_meshes_.clear();
}

GpuMeshId GpuRendererVulkan::acquire_mesh(const std::string& key,
                                          const GpuMeshData& data)
{
	if (!device_valid_ || data.positions == nullptr || data.indices == nullptr ||
	    data.vertex_count == 0 || data.index_count == 0) {
		return kNoGpuMesh;
	}
	if (const auto found = mesh_ids_.find(key); found != mesh_ids_.end()) {
		const GpuMeshId cached_id = found->second;
		CachedMesh& cached        = meshes_[cached_id];
		if (cached.vertex_count == data.vertex_count &&
		    cached.source_index_count == data.index_count) {
			cached.last_used = frames_rendered_;
			return cached_id;
		}
		// The key now names geometry of a different size: replace it.
		release_mesh(cached_id);
	}

	const std::size_t vertex_count = data.vertex_count;
	PendingMeshUpload upload;
	upload.vertices.resize(vertex_count);
	for (std::size_t i = 0; i < vertex_count; ++i) {
		GpuVertex& vertex = upload.vertices[i];
		vertex.position   = data.positions[i];
		// A vertex without a normal is stored as zero; lit.vert turns that
		// into +Z, as the CPU path always did.
		vertex.normal = (data.normals != nullptr && i < data.normal_count)
		                        ? data.normals[i]
		                        : glm::vec3{0.0f};
		// The instance supplies colour and roughness; 1 passes them through.
		vertex.color     = glm::vec3{1.0f};
		vertex.roughness = 1.0f;
	}
	upload.uvs.resize(vertex_count);
	const bool has_uvs = data.uvs != nullptr && data.uv_count == vertex_count;
	for (std::size_t i = 0; i < vertex_count; ++i) {
		upload.uvs[i] = has_uvs ? data.uvs[i] : glm::vec2{0.0f};
	}
	// An index that names no vertex is skipped, exactly as the world-space
	// path always did, so both paths draw the same triangles.
	upload.indices.reserve(data.index_count);
	for (std::size_t i = 0; i < data.index_count; ++i) {
		if (data.indices[i] < vertex_count) {
			upload.indices.push_back(data.indices[i]);
		}
	}
	if (upload.indices.empty()) {
		return kNoGpuMesh;
	}

	CachedMesh mesh;
	mesh.vertex = memory_.create_buffer(
	        vk::MemoryClass::DeviceLocal, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
	                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	        vertex_count * sizeof(GpuVertex), "mesh.vertex");
	mesh.uv = memory_.create_buffer(
	        vk::MemoryClass::DeviceLocal, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
	                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	        vertex_count * sizeof(glm::vec2), "mesh.uv");
	mesh.index = memory_.create_buffer(
	        vk::MemoryClass::DeviceLocal, VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	        upload.indices.size() * sizeof(uint32_t), "mesh.index");
	mesh.vertex_count       = static_cast<uint32_t>(vertex_count);
	mesh.source_index_count = static_cast<uint32_t>(data.index_count);
	mesh.index_count        = static_cast<uint32_t>(upload.indices.size());
	mesh.last_used          = frames_rendered_;
	mesh.key                = key;
	mesh.live               = true;

	GpuMeshId id = kNoGpuMesh;
	if (!free_mesh_ids_.empty()) {
		id = free_mesh_ids_.back();
		free_mesh_ids_.pop_back();
	} else {
		id = static_cast<GpuMeshId>(meshes_.size());
		meshes_.emplace_back();
	}
	meshes_[id] = std::move(mesh);
	mesh_ids_.emplace(key, id);
	upload.id = id;
	pending_meshes_.push_back(std::move(upload));
	return id;
}

void GpuRendererVulkan::upload_pending_meshes(VkCommandBuffer cmd)
{
	if (pending_meshes_.empty()) {
		return;
	}
	std::size_t total = 0;
	for (const PendingMeshUpload& pending : pending_meshes_) {
		total += pending.vertices.size() * sizeof(GpuVertex) +
		         pending.uvs.size() * sizeof(glm::vec2) +
		         pending.indices.size() * sizeof(uint32_t);
	}
	vk::Buffer staging = memory_.create_buffer(vk::MemoryClass::Staging,
	                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                           total, "mesh.upload");
	auto* bytes       = static_cast<uint8_t*>(staging.mapped);
	std::size_t offset = 0;
	const auto put     = [&](const void* source, std::size_t size) {
		std::memcpy(bytes + offset, source, size);
		offset += size;
	};
	for (const PendingMeshUpload& pending : pending_meshes_) {
		put(pending.vertices.data(), pending.vertices.size() * sizeof(GpuVertex));
		put(pending.uvs.data(), pending.uvs.size() * sizeof(glm::vec2));
		put(pending.indices.data(), pending.indices.size() * sizeof(uint32_t));
	}
	memory_.flush(staging.allocation, 0, total);

	vk::BufferState staging_state{};
	buffer_transfer_src(*barriers_, cmd, staging, staging_state);
	offset = 0;
	const auto record = [&](const vk::Buffer& destination,
	                        vk::BufferState& state, std::size_t size) {
		buffer_transfer_dst(*barriers_, cmd, destination, state);
		barriers_->flush(cmd);
		VkBufferCopy copy{};
		copy.srcOffset = offset;
		copy.dstOffset = 0;
		copy.size      = size;
		context_.api.CmdCopyBuffer(cmd, staging.handle, destination.handle,
		                           1, &copy);
		offset += size;
	};
	for (const PendingMeshUpload& pending : pending_meshes_) {
		CachedMesh& mesh = meshes_[pending.id];
		record(mesh.vertex, mesh.vertex_state,
		       pending.vertices.size() * sizeof(GpuVertex));
		record(mesh.uv, mesh.uv_state,
		       pending.uvs.size() * sizeof(glm::vec2));
		record(mesh.index, mesh.index_state,
		       pending.indices.size() * sizeof(uint32_t));
	}
	barriers_->flush(cmd);
	// The staging copy is in flight on this frame's command buffer; it dies
	// with the slot once the slot's fence has been waited on.
	slot_->garbage.push_back([this, staging]() mutable {
		memory_.destroy_buffer(staging);
	});
	pending_meshes_.clear();
}

void GpuRendererVulkan::abandon_geometry_staging()
{
	if (!geometry_mapped_) {
		return;
	}
	geometry_mapped_  = false;
	staged_vertices_  = 0;
	staged_instances_ = 0;
}

void GpuRendererVulkan::begin_geometry_frame()
{
	abandon_geometry_staging();
	// The draw list belongs to the frame that uploaded it: a frame that
	// uploads nothing draws no shadows, and cannot reach a mesh the eviction
	// below has just released.
	geometry_draws_.clear();
	// Cheap enough to check every 64th frame; a scene that stopped drawing
	// still lets its meshes age out.
	if ((frames_rendered_ & 63u) == 0 && !mesh_ids_.empty()) {
		evict_idle_meshes();
	}
}

GpuRendererVulkan::GeometryStaging GpuRendererVulkan::begin_geometry_upload(
        uint32_t immediate_vertices, uint32_t instance_count)
{
	abandon_geometry_staging();
	geometry_draws_.clear();
	if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    (immediate_vertices == 0 && instance_count == 0)) {
		return {};
	}
	grow_streams(*slot_, immediate_vertices, instance_count);

	GeometryStaging staging;
	if (immediate_vertices != 0) {
		staging.vertices = static_cast<GpuVertex*>(slot_->immediate.mapped);
		staging.uvs      = static_cast<glm::vec2*>(slot_->immediate_uv.mapped);
	}
	auto* records = static_cast<GpuInstance*>(slot_->instances.mapped);
	records[0]    = GpuInstance{}; // the identity the immediate stream draws with
	staging.instances = records + kReservedInstances;

	staged_vertices_  = immediate_vertices;
	staged_instances_ = instance_count;
	geometry_mapped_  = true;
	return staging;
}

void GpuRendererVulkan::commit_geometry_upload(const GpuDrawBatch* batches,
                                               uint32_t batch_count)
{
	if (!geometry_mapped_) {
		return;
	}
	geometry_mapped_  = false;
	staged_vertices_  = 0;
	staged_instances_ = 0;

	geometry_draws_.clear();
	for (uint32_t i = 0; i < batch_count; ++i) {
		const GpuDrawBatch& batch = batches[i];
		const bool instanced      = batch.mesh != kNoGpuMesh;
		if (instanced ? (batch.instance_count == 0 || !mesh_live(batch.mesh))
		              : batch.vertex_count == 0) {
			continue;
		}
		geometry_draws_.push_back(GeometryDraw{
		        batch.mesh, batch.first_vertex, batch.vertex_count,
		        batch.first_instance, batch.instance_count});
	}

	// The streams were written straight into device-visible memory: no copy
	// pass, only visibility for non-coherent types (a no-op here) and the
	// transfer-dst barriers below for the mesh uploads.
	memory_.flush(slot_->instances.allocation, 0, VK_WHOLE_SIZE);
	if (slot_->immediate.handle != VK_NULL_HANDLE) {
		memory_.flush(slot_->immediate.allocation, 0, VK_WHOLE_SIZE);
		memory_.flush(slot_->immediate_uv.allocation, 0, VK_WHOLE_SIZE);
	}
	upload_pending_meshes(cmd_);
	// Host-written streams become vertex/uniform reads; mesh uploads became
	// transfer writes above and are transitioned to reads here, so the passes
	// below only bind.
	buffer_vertex_read(*barriers_, cmd_, slot_->instances, slot_->instance_state);
	buffer_vertex_read(*barriers_, cmd_, slot_->immediate, slot_->immediate_state);
	buffer_vertex_read(*barriers_, cmd_, slot_->immediate_uv,
	                   slot_->immediate_uv_state);
	buffer_uniform_read(*barriers_, cmd_, slot_->uniforms, slot_->uniform_state);
	for (const GeometryDraw& draw : geometry_draws_) {
		if (draw.mesh == kNoGpuMesh || !mesh_live(draw.mesh)) {
			continue;
		}
		CachedMesh& mesh = meshes_[draw.mesh];
		buffer_vertex_read(*barriers_, cmd_, mesh.vertex, mesh.vertex_state);
		buffer_vertex_read(*barriers_, cmd_, mesh.uv, mesh.uv_state);
		buffer_index_read(*barriers_, cmd_, mesh.index, mesh.index_state);
	}
	barriers_->flush(cmd_);
}

void GpuRendererVulkan::record_geometry_draw(VkCommandBuffer cmd,
                                             const GeometryDraw& draw)
{
	const VkDeviceSize zero[3]{0, 0, 0};
	if (draw.mesh == kNoGpuMesh) {
		const VkBuffer buffers[3] = {slot_->immediate.handle,
		                             slot_->immediate_uv.handle,
		                             slot_->instances.handle};
		context_.api.CmdBindVertexBuffers(cmd, 0, 3, buffers, zero);
		// Instance 0 is the identity: the stream is already in world space.
		context_.api.CmdDraw(cmd, draw.vertex_count, 1, draw.first_vertex, 0);
		return;
	}
	if (!mesh_live(draw.mesh)) {
		return;
	}
	const CachedMesh& mesh = meshes_[draw.mesh];
	const VkBuffer buffers[3] = {mesh.vertex.handle, mesh.uv.handle,
	                             slot_->instances.handle};
	context_.api.CmdBindVertexBuffers(cmd, 0, 3, buffers, zero);
	context_.api.CmdBindIndexBuffer(cmd, mesh.index.handle, 0,
	                                VK_INDEX_TYPE_UINT32);
	context_.api.CmdDrawIndexed(cmd, mesh.index_count, draw.instance_count, 0,
	                            0, draw.first_instance + kReservedInstances);
}

} // namespace lotus
