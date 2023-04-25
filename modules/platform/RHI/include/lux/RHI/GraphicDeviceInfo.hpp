#pragma once
#include <memory>
#include <string>
#include <vector>
#include <lux/system/visibility_control.h>

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
		LUX_EXPORT static std::vector<GraphicDeviceInfo> getGraphicDeviceInfo();
	};
}
