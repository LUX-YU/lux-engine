#pragma once
#include <memory>
#include <string>
#include <vector>
#include <lux/engine/platform/visibility.h>

namespace lux::rhi
{
	struct DriverInfo
	{
		std::string name;

		std::string version;
	};

	struct GraphicDeviceInfo
	{
		std::string					name;

		std::vector<DriverInfo>		support_driver_list;

		int							using_device_index;
	};

	class GraphicDeviceInfoLoader
	{
	public:

		// TODO
		LUX_PLATFORM_PUBLIC static std::vector<GraphicDeviceInfo> getGraphicDeviceInfo();
	};
}