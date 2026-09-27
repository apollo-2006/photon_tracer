#pragma once
// Surface materials. Each one decides, for a ray that hit it, whether the ray
// scatters, in which direction, and how much of each color channel survives.
#include "hittable.hpp"
#include "vec3.hpp"

#include <cmath>

class material {
public:
    virtual ~material() = default;

    virtual bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const = 0;
};

inline vec3 reflect(const vec3& v, const vec3& n) {
    return v - n * (2.0 * vec3::dot(v, n));
}

// Snell's law for a unit incoming direction; ratio is eta_in / eta_out.
inline vec3 refract(const vec3& uv, const vec3& n, double ratio) {
    double cos_theta = std::fmin(vec3::dot(-uv, n), 1.0);
    vec3 r_perp = (uv + n * cos_theta) * ratio;
    vec3 r_parallel = n * -std::sqrt(std::fabs(1.0 - vec3::dot(r_perp, r_perp)));
    return r_perp + r_parallel;
}

// Matte. Scattering toward normal + a random unit vector gives a cosine
// weighted distribution, which is what a Lambertian surface reflects.
class lambertian : public material {
public:
    color albedo;

    explicit lambertian(const color& a) : albedo(a) {}

    bool scatter(const ray&, const hit_record& rec, color& attenuation, ray& scattered) const override {
        vec3 direction = rec.normal + random_in_unit_sphere().normalize();
        // The random vector can cancel the normal almost exactly.
        if (vec3::dot(direction, direction) < 1e-16) direction = rec.normal;
        scattered = ray(rec.p, direction);
        attenuation = albedo;
        return true;
    }
};

// Mirror, blurred by fuzz: 0 is a perfect mirror, 1 is close to matte.
class metal : public material {
public:
    color albedo;
    double fuzz;

    metal(const color& a, double f) : albedo(a), fuzz(f < 1 ? f : 1) {}

    bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const override {
        vec3 reflected = reflect(r_in.direction().normalize(), rec.normal);
        scattered = ray(rec.p, reflected + random_in_unit_sphere() * fuzz);
        attenuation = albedo;
        // Fuzz can push the ray below the surface; absorb it there.
        return vec3::dot(scattered.direction(), rec.normal) > 0;
    }
};

// Glass, water, diamond. Refracts where it can, reflects where total internal
// reflection forces it, and otherwise picks between the two with Schlick's
// approximation of the Fresnel reflectance.
class dielectric : public material {
public:
    double ior;  // Index of refraction: 1.5 for glass, 1.33 for water

    explicit dielectric(double index) : ior(index) {}

    bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const override {
        attenuation = color(1.0, 1.0, 1.0);
        double ratio = rec.front_face ? (1.0 / ior) : ior;

        vec3 unit_direction = r_in.direction().normalize();
        double cos_theta = std::fmin(vec3::dot(-unit_direction, rec.normal), 1.0);
        double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);

        bool cannot_refract = ratio * sin_theta > 1.0;
        vec3 direction = (cannot_refract || schlick(cos_theta, ratio) > random_double())
                             ? reflect(unit_direction, rec.normal)
                             : refract(unit_direction, rec.normal, ratio);

        scattered = ray(rec.p, direction);
        return true;
    }

private:
    static double schlick(double cosine, double ratio) {
        double r0 = (1 - ratio) / (1 + ratio);
        r0 = r0 * r0;
        return r0 + (1 - r0) * std::pow(1 - cosine, 5);
    }
};
