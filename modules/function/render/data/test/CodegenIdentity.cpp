#include "a/Types.data.hpp"
#include "b/Types.data.hpp"

static_assert(lux::render::GpuData<LeftRecord> && lux::render::GpuData<RightRecord>);
static_assert(
    lux::render::DataSchema<LeftRecord>::descriptor().id != lux::render::DataSchema<RightRecord>::descriptor().id
);

int main()
{
    return lux::render::validateDataSchema(lux::render::DataSchema<LeftRecord>::descriptor()) &&
                   lux::render::validateDataSchema(lux::render::DataSchema<RightRecord>::descriptor())
               ? 0
               : 1;
}
