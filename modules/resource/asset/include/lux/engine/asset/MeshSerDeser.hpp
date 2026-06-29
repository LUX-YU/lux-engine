#pragma once
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>

namespace lux::asset
{
	class LUX_RESOURCE_PUBLIC MeshSerDeser : public TAssetSerDeser<MeshLoadConfig>
	{
		friend class ModelSerDeser;
	public:
		explicit MeshSerDeser(std::shared_ptr<AssetManager> manager);

		/**
		 * @brief Decode a complete .luxasset memory image into PURE mesh data.
		 *
		 * Reads the MESH file header to locate and validate the description
		 * section, then decodes it into a freestanding `Mesh` (MeshAsset's
		 * asset_data_t). This entry point touches ONLY the byte image — it never
		 * dereferences the AssetManager, registers anything, or allocates a
		 * LuxAsset — so it is safe to call from a worker thread during async
		 * loading. The main thread can then inject the returned data into an
		 * already-existing asset shell.
		 *
		 * fromLuxAssetStream() is built on top of this (it decodes the data here,
		 * then wraps it into a MeshAsset), so there is a single decode path.
		 *
		 * @param bytes Pointer to the first byte of the .luxasset image.
		 * @param len   Size of the image in bytes.
		 * @return The decoded Mesh on success, or an EAssetError on failure.
		 */
		[[nodiscard]] static lux::cxx::expected<std::unique_ptr<Mesh>, EAssetError>
		decodeData(const void* bytes, std::size_t len) noexcept;

	protected:
		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromFileStream(std::ifstream& external_path) override;

		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromLuxAssetStream(std::istream& path) override;

		EAssetError
		exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& path) override;
	};
}
