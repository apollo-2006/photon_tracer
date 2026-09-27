#pragma once
// Bounding volume hierarchy. A binary tree of boxes over the scene, so a ray
// tests O(log n) boxes on its way to the few spheres it can actually hit,
// instead of every sphere in the list.
#include "aabb.hpp"
#include "hittable.hpp"

#include <algorithm>
#include <memory>
#include <vector>

class bvh_node : public hittable {
public:
    std::shared_ptr<hittable> left, right;
    aabb box;

    // Builds the tree over objects[start, end), reordering that range. Each
    // node splits at the median along the longest axis of its centroids.
    bvh_node(std::vector<std::shared_ptr<hittable>>& objects, size_t start, size_t end) {
        aabb centroids = centroid_box(objects[start]);
        for (size_t i = start + 1; i < end; ++i)
            centroids = aabb::surrounding(centroids, centroid_box(objects[i]));
        vec3 extent = centroids.max - centroids.min;
        int axis = extent.x() > extent.y() ? (extent.x() > extent.z() ? 0 : 2) : (extent.y() > extent.z() ? 1 : 2);

        size_t count = end - start;
        if (count == 1) {
            left = right = objects[start];
        } else if (count == 2) {
            left = objects[start];
            right = objects[start + 1];
        } else {
            size_t mid = start + count / 2;
            std::nth_element(objects.begin() + start, objects.begin() + mid, objects.begin() + end,
                             [axis](const auto& a, const auto& b) { return center(a, axis) < center(b, axis); });
            left = std::make_shared<bvh_node>(objects, start, mid);
            right = std::make_shared<bvh_node>(objects, mid, end);
        }
        box = aabb::surrounding(left->bounding_box(), right->bounding_box());
    }

    bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override {
        if (!box.hit(r, t_min, t_max)) return false;
        bool hit_left = left->hit(r, t_min, t_max, rec);
        // Only look for hits in the right subtree closer than the left's.
        bool hit_right = right != left && right->hit(r, t_min, hit_left ? rec.t : t_max, rec);
        return hit_left || hit_right;
    }

    aabb bounding_box() const override { return box; }

private:
    static double center(const std::shared_ptr<hittable>& h, int axis) {
        aabb b = h->bounding_box();
        return 0.5 * (b.min.e[axis] + b.max.e[axis]);
    }
    static aabb centroid_box(const std::shared_ptr<hittable>& h) {
        aabb b = h->bounding_box();
        point3 c = (b.min + b.max) * 0.5;
        return aabb(c, c);
    }
};
