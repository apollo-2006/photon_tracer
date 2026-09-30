#pragma once
// Surface materials. Each one decides, for a ray that hit it, whether the ray
// scatters, in which direction, and how much of each color channel survives.
#include "hittable.hpp"
#include "sampler.hpp"
#include "vec3.hpp"

#include <cmath>

class material {
public:
    virtual ~material() = default;

    virtual bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const = 0;

    // Plain fields rather than virtual calls, since the tracer reads them at
    // every bounce. emission: light given off by the front face, zero except
    // for lights. diffuse: a matte surface, which also gathers light by
    // sampling the lights directly (see direct_light() in renderer.hpp).
    color emission;
    bool diffuse = false;
    // Glass: its attenuation is white and says nothing of the surface, so the
    // denoiser takes its albedo as white too.
    bool transmissive = false;

    bool emits() const { return emission.x() > 0 || emission.y() > 0 || emission.z() > 0; }
};

inline vec3 reflect(const vec3& v, const vec3& n) {
    return v - n * (2 * vec3::dot(v, n));
}

// Snell's law for a unit incoming direction; ratio is eta_in / eta_out.
inline vec3 refract(const vec3& uv, const vec3& n, real ratio) {
    real cos_theta = std::fmin(vec3::dot(-uv, n), real(1));
    vec3 r_perp = (uv + n * cos_theta) * ratio;
    vec3 r_parallel = n * -std::sqrt(std::fabs(1 - vec3::dot(r_perp, r_perp)));
    return r_perp + r_parallel;
}

// Matte. Scattering toward normal + a random unit vector gives a cosine
// weighted distribution, which is what a Lambertian surface reflects.
class lambertian : public material {
public:
    color albedo;

    explicit lambertian(const color& a) : albedo(a) { diffuse = true; }

    // Cosine-weighted directions around the normal, drawn from two numbers so
    // that they can come from the path's well-spread sample pairs
    // (sampler.hpp). The same distribution as the normal plus a random unit
    // vector, which needed rejection sampling to find that vector.
    bool scatter(const ray&, const hit_record& rec, color& attenuation, ray& scattered) const override {
        real a, b;
        sample_2d(0, a, b);
        const real r = std::sqrt(a), phi = 2 * real(M_PI) * b;
        const vec3& n = rec.normal;
        // Any two directions perpendicular to n and to each other (Duff et al. 2017).
        const real sign = std::copysign(real(1), n.z());
        const real p = -1 / (sign + n.z()), q = n.x() * n.y() * p;
        const vec3 t1(1 + sign * n.x() * n.x() * p, sign * q, -sign * n.x());
        const vec3 t2(q, sign + n.y() * n.y() * p, -n.y());
        const vec3 direction = t1 * (r * std::cos(phi)) + t2 * (r * std::sin(phi)) +
                               n * std::sqrt(std::fmax(real(0), 1 - a));
        scattered = ray(rec.p, direction);
        attenuation = albedo;
        return true;
    }
};

// Mirror, blurred by fuzz: 0 is a perfect mirror, 1 is close to matte.
class metal : public material {
public:
    color albedo;
    real fuzz;

    metal(const color& a, real f) : albedo(a), fuzz(f < 1 ? f : 1) {}

    bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const override {
        vec3 reflected = reflect(r_in.direction().normalize(), rec.normal);
        scattered = ray(rec.p, reflected + random_in_unit_sphere() * fuzz);
        attenuation = albedo;
        // Fuzz can push the ray below the surface; absorb it there.
        return vec3::dot(scattered.direction(), rec.normal) > 0;
    }
};

// A light: gives off emit from its front face and scatters nothing. Spheres
// made of it are sampled directly from matte surfaces; triangles made of it
// are only found by rays that happen to hit them.
class diffuse_light : public material {
public:
    explicit diffuse_light(const color& emit) { emission = emit; }

    bool scatter(const ray&, const hit_record&, color&, ray&) const override { return false; }
};

// Glass, water, diamond. Refracts where it can, reflects where total internal
// reflection forces it, and otherwise picks between the two with Schlick's
// approximation of the Fresnel reflectance.
class dielectric : public material {
public:
    real ior;  // Index of refraction: 1.5 for glass, 1.33 for water

    explicit dielectric(real index) : ior(index) { transmissive = true; }

    bool scatter(const ray& r_in, const hit_record& rec, color& attenuation, ray& scattered) const override {
        attenuation = color(1, 1, 1);
        real ratio = rec.front_face ? (1 / ior) : ior;

        vec3 unit_direction = r_in.direction().normalize();
        real cos_theta = std::fmin(vec3::dot(-unit_direction, rec.normal), real(1));
        real sin_theta = std::sqrt(1 - cos_theta * cos_theta);

        bool cannot_refract = ratio * sin_theta > 1;
        vec3 direction = (cannot_refract || schlick(cos_theta, ratio) > random_real())
                             ? reflect(unit_direction, rec.normal)
                             : refract(unit_direction, rec.normal, ratio);

        scattered = ray(rec.p, direction);
        return true;
    }

private:
    static real schlick(real cosine, real ratio) {
        real r0 = (1 - ratio) / (1 + ratio);
        r0 = r0 * r0;
        real x = 1 - cosine, x2 = x * x;
        return r0 + (1 - r0) * x2 * x2 * x;
    }
};
