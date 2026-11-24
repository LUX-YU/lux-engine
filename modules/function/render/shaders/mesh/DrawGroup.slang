// Matches the 64-byte MeshDrawGroupRecord supplied by the instance Resource.
// Records/chunk indices address the dense live-group table, not recycled CPU logical slots.
struct DrawGroup
{
    uint index_count;
    uint candidate_count;
    uint first_index;
    int vertex_offset;
    uint candidate_offset;
    uint visible_offset;
    uint visible_stride;
    uint view_count;
    vec4 center;
    vec4 half_extent;
};
