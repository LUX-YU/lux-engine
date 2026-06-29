#pragma once 
#include "lux/engine/gapi/vk/ShaderModule.hpp"
#include <fstream>
#include <vector>

namespace lux::gapi::vk
{
	namespace detail
	{
		static inline std::vector<char> readFile(const std::string& filename) 
		{
			std::ifstream file(filename, std::ios::ate | std::ios::binary);

			if (!file.is_open()) {
				throw std::runtime_error("failed to open file!");
			}

			size_t fileSize = (size_t)file.tellg();
			std::vector<char> buffer(fileSize);

			file.seekg(0);
			file.read(buffer.data(), fileSize);

			file.close();

			return buffer;
		}
	}

	static inline ShaderModule createShaderModule(VkDevice device, const std::string& filename, VkAllocationCallbacks* allocator = nullptr) 
	{
		VkShaderModuleCreateInfo createInfo{};
		auto code = detail::readFile(filename);
		createInfo.sType	= VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();
		createInfo.pCode	= reinterpret_cast<const uint32_t*>(code.data());

		return ShaderModule{ device, createInfo, allocator };
	}
}
