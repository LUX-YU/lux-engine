#include "CubeVertex.hpp"

Eigen::Vector5f cube_vertex[36] = {
    {-50.0f, -50.0f, -50.0f,  0.0f, 0.0f},
    { 50.0f, -50.0f, -50.0f,  1.0f, 0.0f},
    { 50.0f,  50.0f, -50.0f,  1.0f, 1.0f},
    { 50.0f,  50.0f, -50.0f,  1.0f, 1.0f},
    {-50.0f,  50.0f, -50.0f,  0.0f, 1.0f},
    {-50.0f, -50.0f, -50.0f,  0.0f, 0.0f},
    {-50.0f, -50.0f,  50.0f,  0.0f, 0.0f},
    { 50.0f, -50.0f,  50.0f,  1.0f, 0.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 1.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 1.0f},
    {-50.0f,  50.0f,  50.0f,  0.0f, 1.0f},
    {-50.0f, -50.0f,  50.0f,  0.0f, 0.0f},
    {-50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    {-50.0f,  50.0f, -50.0f,  1.0f, 1.0f},
    {-50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    {-50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    {-50.0f, -50.0f,  50.0f,  0.0f, 0.0f},
    {-50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    { 50.0f,  50.0f, -50.0f,  1.0f, 1.0f},
    { 50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    { 50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    { 50.0f, -50.0f,  50.0f,  0.0f, 0.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    {-50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    { 50.0f, -50.0f, -50.0f,  1.0f, 1.0f},
    { 50.0f, -50.0f,  50.0f,  1.0f, 0.0f},
    { 50.0f, -50.0f,  50.0f,  1.0f, 0.0f},
    {-50.0f, -50.0f,  50.0f,  0.0f, 0.0f},
    {-50.0f, -50.0f, -50.0f,  0.0f, 1.0f},
    {-50.0f,  50.0f, -50.0f,  0.0f, 1.0f},
    { 50.0f,  50.0f, -50.0f,  1.0f, 1.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    { 50.0f,  50.0f,  50.0f,  1.0f, 0.0f},
    {-50.0f,  50.0f,  50.0f,  0.0f, 0.0f},
    {-50.0f,  50.0f, -50.0f,  0.0f, 1.0f}
};