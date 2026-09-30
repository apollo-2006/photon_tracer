#pragma once
// Bounding volume hierarchy. A tree of boxes over the scene, so a ray tests
// O(log n) boxes on its way to the few primitives it can actually hit,
// instead of every primitive in the scene.
//
// It is built as a binary tree with the surface area heuristic, which places
// each split where the expected cost of tracing a ray through the two halves is
// lowest, and then collapsed into a 4-wide tree: each node holds four child
// boxes, laid out axis by axis so one SIMD slab test (simd4.hpp) checks all
// four at once. That halves the tree's depth and turns four box tests into
// one. The tree is flat: nodes live in one array, and a leaf is a run of the
// primitive order array.
#include "aabb.hpp"
#include "simd4.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

class bvh {
public:
    // Four children. A child is a leaf when count is nonzero (primitives
    // order[first, first + count)) and otherwise the node at nodes[first].
    // Unused slots have a box at +infinity on every side, which no ray enters,
    // so the traversal needs no separate test for them.
    struct alignas(16) node {
        float lo[3][4];  // lo[axis][child]
        float hi[3][4];
        uint32_t first[4];
        uint32_t count[4];
    };
    static_assert(sizeof(real) == sizeof(float), "the SIMD box test is single precision");

    std::vector<node> nodes;
    std::vector<uint32_t> order;  // Primitive indices, grouped so each leaf is a contiguous run

    // Builds the tree over primitives 0..boxes.size()-1, where boxes[i] bounds primitive i.
    void build(const std::vector<aabb>& boxes) {
        nodes.clear();
        order.resize(boxes.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        if (boxes.empty()) return;
        centers.resize(boxes.size());
        for (size_t i = 0; i < boxes.size(); ++i) centers[i] = boxes[i].center();
        binary.reserve(2 * boxes.size());
        binary.push_back({aabb(), 0, static_cast<uint32_t>(boxes.size())});
        split(0, boxes, 0);
        collapse(0);
        centers = {};
        binary = {};
    }

    // Calls test(primitive, t_max) for each primitive in every leaf the ray
    // reaches, nearer subtrees first. test shrinks t_max when it finds a hit,
    // which prunes every box behind it.
    template <class Test>
    void traverse(const ray& r, real t_min, real& t_max, Test&& test) const {
        if (nodes.empty()) return;
        const point3 o = r.origin();
        const vec3 d = r.direction();
        // A zero component gives infinity, which the slab test handles.
        const f4 ox = splat4(o.x()), oy = splat4(o.y()), oz = splat4(o.z());
        const f4 ix = splat4(1 / d.x()), iy = splat4(1 / d.y()), iz = splat4(1 / d.z());

        // Each visited node pushes at most three more entries than it pops.
        struct entry_t { uint32_t first, count; real t; };
        entry_t stack[3 * max_depth + 1];
        int top = 0;
        stack[top++] = {0, 0, t_min};

        while (top > 0) {
            const entry_t e = stack[--top];
            if (e.t >= t_max) continue;  // Behind the nearest hit found since it was pushed
            if (e.count > 0) {
                for (uint32_t i = e.first; i < e.first + e.count; ++i) test(order[i], t_max);
                continue;
            }

            // Slab test against all four children: clip [t_min, t_max] by the
            // entry and exit distances on each axis.
            const node& nd = nodes[e.first];
            const f4 x0 = (load4(nd.lo[0]) - ox) * ix, x1 = (load4(nd.hi[0]) - ox) * ix;
            const f4 y0 = (load4(nd.lo[1]) - oy) * iy, y1 = (load4(nd.hi[1]) - oy) * iy;
            const f4 z0 = (load4(nd.lo[2]) - oz) * iz, z1 = (load4(nd.hi[2]) - oz) * iz;
            const f4 t_near = max4(max4(min4(x0, x1), min4(y0, y1)), max4(min4(z0, z1), splat4(t_min)));
            const f4 t_far = min4(min4(max4(x0, x1), max4(y0, y1)), min4(max4(z0, z1), splat4(t_max)));
            const int mask = le_mask(t_near, t_far);
            if (mask == 0) continue;

            // Push the children that were hit farthest first, so the nearest
            // is popped next.
            alignas(16) float enter[4];
            store4(enter, t_near);
            entry_t hit[4];
            int n = 0;
            for (int c = 0; c < 4; ++c) {
                if (!(mask & (1 << c))) continue;
                entry_t h = {nd.first[c], nd.count[c], enter[c]};
                int k = n++;
                for (; k > 0 && hit[k - 1].t < h.t; --k) hit[k] = hit[k - 1];
                hit[k] = h;
            }
            for (int k = 0; k < n; ++k) stack[top++] = hit[k];
        }
    }

private:
    struct binary_node {
        aabb box;
        uint32_t first;  // Interior: index of the left child, the right is first + 1. Leaf: start in order.
        uint32_t count;  // Primitives in a leaf, 0 for an interior node
    };

    // The traversal stack is a fixed array; build() stops splitting at this depth.
    static constexpr int max_depth = 64;
    static constexpr int bins = 16;
    static constexpr int max_leaf = 8;
    // Relative costs for the SAH: stepping into a node versus testing one primitive.
    static constexpr real traversal_cost = 1, intersection_cost = 1;

    // Only during build()
    std::vector<binary_node> binary;
    std::vector<point3> centers;

    void split(uint32_t index, const std::vector<aabb>& boxes, int depth) {
        const uint32_t first = binary[index].first, count = binary[index].count;
        aabb box, centroids;
        for (uint32_t i = first; i < first + count; ++i) {
            box.grow(boxes[order[i]]);
            centroids.grow(aabb(centers[order[i]], centers[order[i]]));
        }
        binary[index].box = box;
        if (count == 1 || depth + 1 >= max_depth) return;

        // Binned SAH: drop the centroids into equal-width bins along each axis
        // and price every boundary between bins as a split.
        real best_cost = aabb::inf();
        int best_axis = -1, best_bin = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const real lo = centroids.min.e[axis], extent = centroids.max.e[axis] - lo;
            if (extent <= 0) continue;  // Every centroid in one plane: nothing to split
            aabb bin_box[bins];
            uint32_t bin_count[bins] = {};
            for (uint32_t i = first; i < first + count; ++i) {
                int b = bin_of(centers[order[i]].e[axis], lo, extent);
                bin_box[b].grow(boxes[order[i]]);
                ++bin_count[b];
            }
            // Sweep from the right, then from the left, so each boundary is priced in O(bins).
            real right_area[bins];
            uint32_t right_count[bins];
            aabb acc;
            uint32_t n = 0;
            for (int b = bins - 1; b > 0; --b) {
                acc.grow(bin_box[b]);
                n += bin_count[b];
                right_area[b] = acc.half_area();
                right_count[b] = n;
            }
            acc = aabb();
            n = 0;
            for (int b = 0; b < bins - 1; ++b) {
                acc.grow(bin_box[b]);
                n += bin_count[b];
                if (n == 0 || right_count[b + 1] == 0) continue;
                real cost = traversal_cost + intersection_cost *
                            (acc.half_area() * n + right_area[b + 1] * right_count[b + 1]) / box.half_area();
                if (cost < best_cost) { best_cost = cost; best_axis = axis; best_bin = b; }
            }
        }

        // Stay a leaf when no split is cheaper than testing everything here,
        // unless that leaf would be longer than max_leaf.
        if (count <= max_leaf && !(best_cost < count * intersection_cost)) return;

        uint32_t mid;
        if (best_axis >= 0) {
            const real lo = centroids.min.e[best_axis], extent = centroids.max.e[best_axis] - lo;
            uint32_t* m = std::partition(order.data() + first, order.data() + first + count, [&](uint32_t p) {
                return bin_of(centers[p].e[best_axis], lo, extent) <= best_bin;
            });
            mid = static_cast<uint32_t>(m - order.data());
        } else {
            // Every centroid coincides, so no bin boundary separates them and
            // any halving is as good as another.
            mid = first + count / 2;
        }

        const uint32_t left = static_cast<uint32_t>(binary.size());
        binary.push_back({aabb(), first, mid - first});
        binary.push_back({aabb(), mid, first + count - mid});
        binary[index].first = left;
        binary[index].count = 0;
        split(left, boxes, depth + 1);
        split(left + 1, boxes, depth + 1);
    }

    // Makes a 4-wide node out of the binary subtree at b and returns its index.
    // Starting from b alone, the interior node with the largest surface area
    // is repeatedly replaced by its two children, until there are four or
    // only leaves are left: large boxes are the ones rays enter most often,
    // so they are the ones worth opening up. The result is never deeper than
    // the binary tree, which keeps the traversal stack within its bound.
    uint32_t collapse(uint32_t b) {
        uint32_t child[4] = {b};
        int n = 1;
        while (n < 4) {
            int widest = -1;
            real widest_area = -1;
            for (int i = 0; i < n; ++i) {
                const binary_node& c = binary[child[i]];
                if (c.count == 0 && c.box.half_area() > widest_area) { widest = i; widest_area = c.box.half_area(); }
            }
            if (widest < 0) break;
            const uint32_t left = binary[child[widest]].first;
            child[widest] = left;
            child[n++] = left + 1;
        }

        const uint32_t index = static_cast<uint32_t>(nodes.size());
        nodes.emplace_back();
        node nd;
        const float inf = std::numeric_limits<float>::infinity();
        for (int i = 0; i < 4; ++i) {
            for (int a = 0; a < 3; ++a) {
                nd.lo[a][i] = i < n ? binary[child[i]].box.min.e[a] : inf;
                nd.hi[a][i] = i < n ? binary[child[i]].box.max.e[a] : inf;
            }
            nd.first[i] = 0;
            nd.count[i] = 0;
            if (i >= n) continue;
            if (binary[child[i]].count > 0) {
                nd.first[i] = binary[child[i]].first;
                nd.count[i] = binary[child[i]].count;
            } else {
                nd.first[i] = collapse(child[i]);  // Appends to nodes, so nd is stored by index below
            }
        }
        nodes[index] = nd;
        return index;
    }

    static int bin_of(real c, real lo, real extent) {
        int b = static_cast<int>(bins * (c - lo) / extent);
        return b < 0 ? 0 : b >= bins ? bins - 1 : b;
    }
};
