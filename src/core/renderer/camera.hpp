#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

constexpr float YAW = -90.0f;
constexpr float PITCH = 0.0f;
namespace lotus
{
    class Camera
    {
    public:
        Camera(glm::vec3 pos = glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f), float yaw = YAW, float pitch = PITCH)
            : pos_{pos},
              up_{up},
              front_{glm::vec3{0.0f, 0.0f, -1.0f}},
              yaw_{yaw},
              pitch_{pitch}
        {
            update_vectors();
        }

        void set_pos(glm::vec3 pos)
        {
            pos_ = pos;
        }

        glm::vec3 get_pos()
        {
            return pos_;
        }

        glm::mat4 view_matrix() const
        {
            return glm::lookAt(pos_, pos_ + front_, up_);
        }

    private:
        void update_vectors()
        {
            front_.x = cos(glm::radians(yaw_)) * cos(glm::radians(pitch_));
            front_.y = sin(glm::radians(pitch_));
            front_.z = sin(glm::radians(yaw_)) * cos(glm::radians(pitch_));
            front_ = glm::normalize(front_);
        }
        glm::vec3 pos_, up_, front_;
        float yaw_, pitch_;
    };
}