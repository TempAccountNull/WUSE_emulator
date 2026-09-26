// The descriptor-copy test binds two storage buffers at set 0 and reads the copied source binding.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> source : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> result : register(u1, space0);
[numthreads(1, 1, 1)]
void main()
{
    result[0] = source[0];
}
