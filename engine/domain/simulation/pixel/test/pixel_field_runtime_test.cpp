#include <lux/engine/simulation/pixel/PixelFieldRuntime.hpp>

#include <cassert>
#include <cstdint>
#include <utility>

namespace
{
    using namespace lux::simulation;

    [[nodiscard]] std::unique_ptr<PixelFieldRuntime> makeField(
        std::uint32_t width,
        std::uint32_t height
    )
    {
        auto created = PixelFieldRuntime::create({width, height});
        assert(created);
        return std::move(*created);
    }

    [[nodiscard]] PixelMaterialId addSand(PixelFieldRuntime& field)
    {
        auto material = field.addMaterial({
            .phase = EPixelMaterialPhase::POWDER,
            .density = 200U,
            .rgba8 = 0xD2B48CFFU,
        });
        assert(material);
        return *material;
    }
}

int main()
{
    {
        auto field = makeField(1024U, 1024U);
        const auto sand = addSand(*field);
        assert(field->setCell(10, 10, sand));
        assert(field->residentChunkCount() == 1U);

        field->step();
        auto source = field->cell(10, 10);
        auto destination = field->cell(10, 11);
        assert(source && *source == kEmptyPixelMaterial);
        assert(destination && *destination == sand);
        assert(field->movedCellsLastStep() == 1U);
        assert(field->cellsScannedLastStep() == 32U * 32U);
    }

    {
        auto left = makeField(1024U, 1024U);
        auto right = makeField(1024U, 1024U);
        const auto left_sand = addSand(*left);
        const auto right_sand = addSand(*right);
        assert(left_sand == right_sand);

        for (std::int64_t x = 40; x < 72; ++x)
        {
            assert(left->setCell(x, 40, left_sand));
            assert(right->setCell(x, 40, right_sand));
        }
        for (std::uint32_t step = 0U; step < 32U; ++step)
        {
            left->step();
            right->step();
            assert(left->determinismHash() == right->determinismHash());
        }
    }

    {
        auto sparse = makeField(8192U, 8192U);
        const auto sand = addSand(*sparse);
        assert(sparse->setCell(1, 1, sand));
        assert(sparse->setCell(7000, 7000, sand));
        assert(sparse->residentChunkCount() == 2U);
        sparse->step();
        assert(sparse->cellsScannedLastStep() == 2U * 32U * 32U);
    }

    {
        auto boundary = makeField(1024U, 1024U);
        const auto sand = addSand(*boundary);
        // Only chunk (0,0) is resident. The three downward destinations from
        // this corner are in missing chunks, so residency is a solid boundary.
        assert(boundary->setCell(255, 255, sand));
        boundary->step();
        auto value = boundary->cell(255, 255);
        assert(value && *value == sand);
        assert(boundary->residentChunkCount() == 1U);
        assert(boundary->movedCellsLastStep() == 0U);
    }

    {
        auto field = makeField(64U, 64U);
        const auto sand = addSand(*field);
        const auto invalid = field->setCell(-1, 0, sand);
        assert(!invalid && invalid.error() == EPixelFieldError::OUT_OF_BOUNDS);
        const auto bad_material = field->setCell(0, 0, static_cast<PixelMaterialId>(60000U));
        assert(!bad_material && bad_material.error() == EPixelFieldError::INVALID_MATERIAL);
    }

    return 0;
}
