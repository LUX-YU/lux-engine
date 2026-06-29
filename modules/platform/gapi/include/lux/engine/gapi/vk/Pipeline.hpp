#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vector>

namespace lux::gapi::vk
{
	class PipelineLayoutBuilder;
	class PipelineLayout
	{
	public:
		using Builder = PipelineLayoutBuilder;

		PipelineLayout() : layout(VK_NULL_HANDLE) {}

		PipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreatePipelineLayout, "Failed to create PipelineLayout object", device, &info, allocator, &layout);
		}

		PipelineLayout(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			VkPipelineLayoutCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			info.flags = 0;
			info.pNext = nullptr;
			info.setLayoutCount = 0;
			info.pSetLayouts = nullptr;
			info.pushConstantRangeCount = 0;
			info.pPushConstantRanges = nullptr;

			VK_FUNC_INVOKE(vkCreatePipelineLayout, "Failed to create PipelineLayout object", device, &info, allocator, &layout);
		}

		PipelineLayout(const PipelineLayout&) = delete;
		PipelineLayout& operator=(const PipelineLayout&) = delete;

		PipelineLayout(PipelineLayout&& other) noexcept
		{
			layout = other.layout;
			other.layout = VK_NULL_HANDLE;
		}

		PipelineLayout& operator=(PipelineLayout&& other) noexcept
		{
			layout = other.layout;
			other.layout = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (layout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(device, layout, allocator);
				layout = VK_NULL_HANDLE;
			}
		}

		inline operator VkPipelineLayout() const noexcept { return layout; }
		inline const VkPipelineLayout* operator&() const noexcept { return &layout; }

		inline VkPipelineLayout handle() const noexcept { return layout; }
		inline const VkPipelineLayout* handlePtr() const noexcept { return &layout; }

	private:
		VkPipelineLayout layout{ VK_NULL_HANDLE };
	};

	class PipelineLayoutBuilder
	{
	public:
		PipelineLayoutBuilder()
		{
			info.sType					= VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			info.flags					= 0;
			info.pNext					= nullptr;
			info.setLayoutCount			= 0;
			info.pSetLayouts			= nullptr;
			info.pushConstantRangeCount = 0;
			info.pPushConstantRanges	= nullptr;
		}

		PipelineLayoutBuilder& setLayouts(const std::vector<VkDescriptorSetLayout>& layouts)
		{
			info.setLayoutCount = static_cast<uint32_t>(layouts.size());
			info.pSetLayouts    = layouts.data();
			return *this;
		}

		PipelineLayoutBuilder& setPushConstantRanges(const std::vector<VkPushConstantRange>& push_constant_ranges)
		{
			info.pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size());
			info.pPushConstantRanges = push_constant_ranges.data();
			return *this;
		}

		PipelineLayout build(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			return PipelineLayout(device, info, allocator);
		}

	private:
		VkPipelineLayoutCreateInfo info;
	};

	class GraphicsPipelineBuilder;
	class Pipeline
	{
	public:
		Pipeline() : pipeline(VK_NULL_HANDLE) {}

		Pipeline(VkDevice device, const VkGraphicsPipelineCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateGraphicsPipelines, "Failed to create Pipeline object", device, VK_NULL_HANDLE, 1, &info, allocator, &pipeline);
		}

		Pipeline(VkDevice device, const VkComputePipelineCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateComputePipelines, "Failed to create Pipeline object", device, VK_NULL_HANDLE, 1, &info, allocator, &pipeline);
		}

		Pipeline(const Pipeline&) = delete;
		Pipeline& operator=(const Pipeline&) = delete;

		Pipeline(Pipeline&& other) noexcept
		{
			pipeline = other.pipeline;
			other.pipeline = VK_NULL_HANDLE;
		}

		Pipeline& operator=(Pipeline&& other) noexcept
		{
			pipeline = other.pipeline;
			other.pipeline = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (pipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(device, pipeline, allocator);
				pipeline = VK_NULL_HANDLE;
			}
		}

		inline operator VkPipeline() const noexcept { return pipeline; }
		inline const VkPipeline* operator&() const noexcept { return &pipeline; }

		inline VkPipeline handle() const noexcept { return pipeline; }
		inline const VkPipeline* handlePtr() const noexcept { return &pipeline; }

	private:
		VkPipeline pipeline{ VK_NULL_HANDLE };
	};

	/* 
		typedef struct VkGraphicsPipelineCreateInfo 
		{
			VkStructureType                                sType;                     
			/// Specifies the type of this structure. For VkGraphicsPipelineCreateInfo, it should be VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO.

			const void*                                    pNext;                     
			/// Pointer to extension-specific information, usually set to NULL.

			VkPipelineCreateFlags                          flags;                     
			/// Flags specifying options for pipeline creation, such as VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT or VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT.

			uint32_t                                       stageCount;                
			/// The number of entries in the pStages array.

			const VkPipelineShaderStageCreateInfo*         pStages;                   
			/// Pointer to an array of VkPipelineShaderStageCreateInfo structures defining the shader stages.

			const VkPipelineVertexInputStateCreateInfo*    pVertexInputState;         
			/// Pointer to a VkPipelineVertexInputStateCreateInfo structure defining vertex input state, including vertex binding and attribute descriptions.

			const VkPipelineInputAssemblyStateCreateInfo*  pInputAssemblyState;       
			/// Pointer to a VkPipelineInputAssemblyStateCreateInfo structure defining input assembly state, including primitive topology and restart index enabling.

			const VkPipelineTessellationStateCreateInfo*   pTessellationState;        
			/// Pointer to a VkPipelineTessellationStateCreateInfo structure defining tessellation state, mainly used to specify the number of control points. Used only if tessellation shaders are used.

			const VkPipelineViewportStateCreateInfo*       pViewportState;            
			/// Pointer to a VkPipelineViewportStateCreateInfo structure defining the number and details of viewports and scissor rectangles.

			const VkPipelineRasterizationStateCreateInfo*  pRasterizationState;       
			/// Pointer to a VkPipelineRasterizationStateCreateInfo structure defining rasterization state, including fill mode, cull mode, depth bias, etc.

			const VkPipelineMultisampleStateCreateInfo*    pMultisampleState;         
			/// Pointer to a VkPipelineMultisampleStateCreateInfo structure defining multisample state, including sample count and sample mask.

			const VkPipelineDepthStencilStateCreateInfo*   pDepthStencilState;        
			/// Pointer to a VkPipelineDepthStencilStateCreateInfo structure defining depth and stencil test state, including depth test, depth write, and stencil test.

			const VkPipelineColorBlendStateCreateInfo*     pColorBlendState;          
			/// Pointer to a VkPipelineColorBlendStateCreateInfo structure defining color blend state, including blend attachments and blend constants.

			const VkPipelineDynamicStateCreateInfo*        pDynamicState;             
			/// Pointer to a VkPipelineDynamicStateCreateInfo structure specifying the set of dynamic states that can be changed during command buffer execution.

			VkPipelineLayout                               layout;                    
			/// Handle to a pipeline layout object, describing the resource bindings used by the pipeline.

			VkRenderPass                                   renderPass;                
			/// Handle to a render pass object, specifying the render pass within which the pipeline will be used.

			uint32_t                                       subpass;                   
			/// Index of the subpass within the render pass where this pipeline will be used.

			VkPipeline                                     basePipelineHandle;        
			/// Handle to an existing pipeline that this pipeline will derive from.

			int32_t                                        basePipelineIndex;         
			/// Index of the pipeline within the create array that this pipeline will derive from. Mainly used for pipeline derivatives.
		} VkGraphicsPipelineCreateInfo;
	*/

	class GraphicsPipelineBuilder
	{
	public:
		GraphicsPipelineBuilder()
		{
			info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			info.flags = 0;
			info.pNext = nullptr;
			info.basePipelineHandle = VK_NULL_HANDLE;
			info.basePipelineIndex = -1;
		}

		GraphicsPipelineBuilder& addShaderStage(const VkPipelineShaderStageCreateInfo& shader_stage)
		{
			shader_stages.push_back(shader_stage);
			return *this;
		}

		GraphicsPipelineBuilder& setInputAssemblyState(const VkPipelineInputAssemblyStateCreateInfo& input_assembly_state)
		{
			info.pInputAssemblyState = &input_assembly_state;
			return *this;
		}

		GraphicsPipelineBuilder& setVertexInputState(const VkPipelineVertexInputStateCreateInfo& vertex_input_state)
		{
			info.pVertexInputState = &vertex_input_state;
			return *this;
		}

		GraphicsPipelineBuilder& setViewportState(const VkPipelineViewportStateCreateInfo& viewport_state)
		{
			info.pViewportState = &viewport_state;
			return *this;
		}

		GraphicsPipelineBuilder& setRasterizationState(const VkPipelineRasterizationStateCreateInfo& rasterization_state)
		{
			info.pRasterizationState = &rasterization_state;
			return *this;
		}

		GraphicsPipelineBuilder& setMultisampleState(const VkPipelineMultisampleStateCreateInfo& multisample_state)
		{
			info.pMultisampleState = &multisample_state;
			return *this;
		}

		GraphicsPipelineBuilder& setDepthStencilState(const VkPipelineDepthStencilStateCreateInfo& depth_stencil_state)
		{
			info.pDepthStencilState = &depth_stencil_state;
			return *this;
		}

		GraphicsPipelineBuilder& setColorBlendState(const VkPipelineColorBlendStateCreateInfo& color_blend_state)
		{
			info.pColorBlendState = &color_blend_state;
			return *this;
		}

		GraphicsPipelineBuilder& setDynamicState(const VkPipelineDynamicStateCreateInfo& dynamic_state)
		{
			info.pDynamicState = &dynamic_state;
			return *this;
		}

		GraphicsPipelineBuilder& setLayout(VkPipelineLayout layout)
		{
			info.layout = layout;
			return *this;
		}

		GraphicsPipelineBuilder& setSubpass(uint32_t subpass)
		{
			info.subpass = subpass;
			return *this;
		}

		GraphicsPipelineBuilder& setBasePipelineHandle(VkPipeline base_pipeline_handle)
		{
			info.basePipelineHandle = base_pipeline_handle;
			return *this;
		}

		GraphicsPipelineBuilder& setBasePipelineIndex(int32_t base_pipeline_index)
		{
			info.basePipelineIndex = base_pipeline_index;
			return *this;
		}

		Pipeline build(VkDevice device, VkRenderPass render_pass, VkAllocationCallbacks* allocator = nullptr) const
		{
			info.stageCount = static_cast<uint32_t>(shader_stages.size());
			info.pStages	= shader_stages.data();
			info.renderPass = render_pass;

			return Pipeline(device, info, allocator);
		}

	protected:
		std::vector<VkPipelineShaderStageCreateInfo>		shader_stages;
		mutable VkGraphicsPipelineCreateInfo				info{};
	};
}