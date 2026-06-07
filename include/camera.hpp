#pragma once
#include "vec3.hpp"
#include "ray.hpp"

class camera {
private:
    point3 origin;
    point3 lower_left_corner;
    vec3 horizontal;
    vec3 vertical;

public:
    camera() {
        const double aspect_ratio = 16.0 / 9.0;
        const double viewport_height = 2.0;
        const double viewport_width = aspect_ratio * viewport_height;
        const double focal_length = 1.0; // Distance from camera to the projection plane

        origin = point3(0, 0, 0);
        horizontal = vec3(viewport_width, 0.0, 0.0);
        vertical = vec3(0.0, viewport_height, 0.0);

        // Calculate the bottom left corner of our viewport screen
        lower_left_corner = origin - (horizontal * 0.5) - (vertical * 0.5) - vec3(0, 0, focal_length);
    }

    // Casts a mathematical ray from the origin through a specific UV coordinate on the screen
    ray get_ray(double u, double v) const {
        return ray(origin, lower_left_corner + (horizontal * u) + (vertical * v) - origin);
    }
};