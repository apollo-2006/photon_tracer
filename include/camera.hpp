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
    // The original view: at the origin, looking down -z, 90 degree vertical field of view.
    camera() : camera(point3(0, 0, 0), point3(0, 0, -1), 90.0) {}

    // A camera at lookfrom aimed at lookat, with vfov_deg of vertical field of
    // view and world up as up. The projection plane sits one unit in front.
    camera(point3 lookfrom, point3 lookat, double vfov_deg) {
        const double aspect_ratio = 16.0 / 9.0;
        const double viewport_height = 2.0 * std::tan(vfov_deg * M_PI / 360.0);
        const double viewport_width = aspect_ratio * viewport_height;

        // Orthonormal basis: w points back from the view, u right, v up.
        vec3 w = (lookfrom - lookat).normalize();
        vec3 u = cross(vec3(0, 1, 0), w).normalize();
        vec3 v = cross(w, u);

        origin = lookfrom;
        horizontal = u * viewport_width;
        vertical = v * viewport_height;

        // Calculate the bottom left corner of our viewport screen
        lower_left_corner = origin - (horizontal * 0.5) - (vertical * 0.5) - w;
    }

    // Casts a mathematical ray from the origin through a specific UV coordinate on the screen
    ray get_ray(real u, real v) const {
        return ray(origin, lower_left_corner + (horizontal * u) + (vertical * v) - origin);
    }
};