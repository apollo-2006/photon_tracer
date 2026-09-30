#version 460
// Writes which instance and triangle covers the pixel: the visibility buffer.
#extension GL_EXT_mesh_shader : require

layout(location = 0) perprimitiveEXT flat in uvec2 id;
layout(location = 0) out uvec2 visibility;

void main() { visibility = id; }
