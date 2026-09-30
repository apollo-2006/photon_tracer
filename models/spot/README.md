# Spot

Keenan Crane's cow, from his [3D model repository](https://www.cs.cmu.edu/~kmcrane/Projects/ModelRepository/),
released by him into the public domain.

`spot.obj` is his `spot_triangulated.obj` with two lines added at the top, `mtllib spot.mtl`
and `usemtl spot`, so it picks up `spot.mtl`, which is written for this renderer: a matte
material colored by `spot_texture.png`, his texture unchanged.

```bash
./photon_tracer --obj models/spot/spot.obj --turn 150
```
