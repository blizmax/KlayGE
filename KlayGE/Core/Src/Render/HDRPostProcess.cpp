// HDRPostProcess.cpp
// KlayGE HDR后期处理类 实现文件
// Ver 3.4.0
// 版权所有(C) 龚敏敏, 2006
// Homepage: http://klayge.sourceforge.net
//
// 3.4.0
// 初次建立 (2006.8.1)
//
// 修改记录
//////////////////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>
#include <KlayGE/Util.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/Math.hpp>
#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/FrameBuffer.hpp>

#include <boost/assert.hpp>

#include <KlayGE/PostProcess.hpp>
#include <KlayGE/HDRPostProcess.hpp>

namespace KlayGE
{
	SumLumPostProcess::SumLumPostProcess(std::string const & tech)
			: PostProcess(Context::Instance().RenderFactoryInstance().LoadEffect("SumLum.fxml")->TechniqueByName(tech))
	{
		tex_coord_offset_ep_ = technique_->Effect().ParameterByName("tex_coord_offset");
	}

	SumLumPostProcess::~SumLumPostProcess()
	{
	}

	void SumLumPostProcess::Source(TexturePtr const & src_tex, bool flipping)
	{
		PostProcess::Source(src_tex, flipping);

		this->GetSampleOffsets4x4(src_tex->Width(0), src_tex->Height(0));
	}

	void SumLumPostProcess::GetSampleOffsets4x4(uint32_t width, uint32_t height)
	{
		std::vector<float4> tex_coord_offset(2);

		float const tu = 1.0f / width;
		float const tv = 1.0f / height;

		// Sample from the 16 surrounding points.
		int index = 0;
		for (int y = -1; y <= 2; y += 2)
		{
			for (int x = -1; x <= 2; x += 4)
			{
				tex_coord_offset[index].x() = (x + 0) * tu;
				tex_coord_offset[index].y() = y * tv;
				tex_coord_offset[index].z() = (x + 2) * tu;
				tex_coord_offset[index].w() = y * tv;

				++ index;
			}
		}

		*tex_coord_offset_ep_ = tex_coord_offset;
	}


	SumLumLogPostProcess::SumLumLogPostProcess()
			: SumLumPostProcess("SumLumLog")
	{
	}

	SumLumIterativePostProcess::SumLumIterativePostProcess()
			: SumLumPostProcess("SumLumIterative")
	{
	}


	AdaptedLumPostProcess::AdaptedLumPostProcess()
			: PostProcess(Context::Instance().RenderFactoryInstance().LoadEffect("SumLum.fxml")->TechniqueByName("AdaptedLum")),
				last_index_(false)
	{
		RenderFactory& rf = Context::Instance().RenderFactoryInstance();

		std::vector<float> data_v(4, 0);
		for (int i = 0; i < 2; ++ i)
		{
			fb_[i] = rf.MakeFrameBuffer();
				
			ElementInitData init_data;
			init_data.row_pitch = sizeof(float);
			init_data.slice_pitch = 0;
			init_data.data = &data_v[0];

			try
			{
				adapted_textures_[i] = rf.MakeTexture2D(1, 1, 1, 1, EF_R32F, 1, 0, EAH_GPU_Read | EAH_GPU_Write, &init_data);
				fb_[i]->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*adapted_textures_[i], 0, 0));
			}
			catch (...)
			{
				init_data.row_pitch = 4 * sizeof(float);
				adapted_textures_[i] = rf.MakeTexture2D(1, 1, 1, 1, EF_ABGR32F, 1, 0, EAH_GPU_Read | EAH_GPU_Write, &init_data);
				fb_[i]->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*adapted_textures_[i], 0, 0));
			}
		}

		this->Destinate(fb_[last_index_]);

		last_lum_tex_ep_ = technique_->Effect().ParameterByName("last_lum_tex");
		frame_delta_ep_ = technique_->Effect().ParameterByName("frame_delta");
	}

	void AdaptedLumPostProcess::Apply()
	{
		this->Destinate(fb_[!last_index_]);

		PostProcess::Apply();
	}

	void AdaptedLumPostProcess::OnRenderBegin()
	{
		PostProcess::OnRenderBegin();

		*last_lum_tex_ep_ = adapted_textures_[last_index_];
		*frame_delta_ep_ = float(timer_.elapsed());
		timer_.restart();

		last_index_ = !last_index_;
	}

	TexturePtr AdaptedLumPostProcess::AdaptedLum() const
	{
		return adapted_textures_[last_index_];
	}


	ToneMappingPostProcess::ToneMappingPostProcess(bool blue_shift)
			: PostProcess(Context::Instance().RenderFactoryInstance().LoadEffect("ToneMapping.fxml")->TechniqueByName("ToneMapping20"))
	{
		RenderEffect const & effect = technique_->Effect();
		if (effect.TechniqueByName("ToneMapping30")->Validate())
		{
			technique_ = effect.TechniqueByName("ToneMapping30");
		}

		lum_tex_ep_ = technique_->Effect().ParameterByName("lum_tex");
		bloom_tex_ep_ = technique_->Effect().ParameterByName("bloom_tex");
		*(technique_->Effect().ParameterByName("blue_shift")) = blue_shift;
	}

	void ToneMappingPostProcess::SetTexture(TexturePtr const & lum_tex, TexturePtr const & bloom_tex)
	{
		*lum_tex_ep_ = lum_tex;
		*bloom_tex_ep_ = bloom_tex;
	}


	HDRPostProcess::HDRPostProcess(bool bright_pass, bool blue_shift)
		: PostProcess(RenderTechniquePtr())
	{
		if (bright_pass)
		{
			downsampler_ = MakeSharedPtr<BrightPassDownsampler2x2PostProcess>();
		}
		else
		{
			downsampler_ = MakeSharedPtr<Downsampler2x2PostProcess>();
		}

		blur_ = MakeSharedPtr<BlurPostProcess<SeparableGaussianFilterPostProcess> >(8, 2.0f);
		sum_lums_1st_ = MakeSharedPtr<SumLumLogPostProcess>();
		sum_lums_.resize(NUM_TONEMAP_TEXTURES);
		for (size_t i = 0; i < sum_lums_.size(); ++ i)
		{
			sum_lums_[i] = MakeSharedPtr<SumLumIterativePostProcess>();
		}
		adapted_lum_ = MakeSharedPtr<AdaptedLumPostProcess>();
		tone_mapping_ = MakeSharedPtr<ToneMappingPostProcess>(blue_shift);
	}

	void HDRPostProcess::Source(TexturePtr const & tex, bool flipping)
	{
		PostProcess::Source(tex, flipping);

		uint32_t const width = tex->Width(0);
		uint32_t const height = tex->Height(0);

		RenderFactory& rf = Context::Instance().RenderFactoryInstance();

		downsample_tex_ = rf.MakeTexture2D(width / 2, height / 2, 1, 1, EF_ABGR16F, 1, 0,
			EAH_GPU_Read | EAH_GPU_Write, NULL);
		bool tmp_flipping;
		{
			FrameBufferPtr fb = rf.MakeFrameBuffer();
			fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*downsample_tex_, 0, 0));
			downsampler_->Source(src_texture_, flipping);
			downsampler_->Destinate(fb);
			tmp_flipping = fb->RequiresFlipping();
		}

		blur_tex_ = rf.MakeTexture2D(width / 4, height / 4, 1, 1, EF_ABGR16F, 1, 0,
			EAH_GPU_Read | EAH_GPU_Write, NULL);
		{
			FrameBufferPtr fb = rf.MakeFrameBuffer();
			fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*blur_tex_, 0, 0));
			blur_->Source(downsample_tex_, tmp_flipping);
			blur_->Destinate(fb);
			tmp_flipping = fb->RequiresFlipping();
		}

		lum_texs_.resize(sum_lums_.size() + 1);
		try
		{
			int len = 1;
			for (size_t i = 0; i < sum_lums_.size() + 1; ++ i)
			{
				lum_texs_[sum_lums_.size() - i] = rf.MakeTexture2D(len, len, 1, 1, EF_R16F, 1, 0,
					EAH_GPU_Read | EAH_GPU_Write, NULL);
				len *= 4;
			}

			{
				FrameBufferPtr fb = rf.MakeFrameBuffer();
				fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*lum_texs_[0], 0, 0));
				sum_lums_1st_->Source(src_texture_, tmp_flipping);
				sum_lums_1st_->Destinate(fb);
				tmp_flipping = fb->RequiresFlipping();
			}
			for (size_t i = 0; i < sum_lums_.size(); ++ i)
			{
				FrameBufferPtr fb = rf.MakeFrameBuffer();
				fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*lum_texs_[i + 1], 0, 0));
				sum_lums_[i]->Source(lum_texs_[i], tmp_flipping);
				sum_lums_[i]->Destinate(fb);
				tmp_flipping = fb->RequiresFlipping();
			}
		}
		catch (...)
		{
			int len = 1;
			for (size_t i = 0; i < sum_lums_.size() + 1; ++ i)
			{
				lum_texs_[sum_lums_.size() - i] = rf.MakeTexture2D(len, len, 1, 1, EF_ABGR16F, 1, 0,
					EAH_GPU_Read | EAH_GPU_Write, NULL);
				len *= 4;
			}

			{
				FrameBufferPtr fb = rf.MakeFrameBuffer();
				fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*lum_texs_[0], 0, 0));
				sum_lums_1st_->Source(src_texture_, tmp_flipping);
				sum_lums_1st_->Destinate(fb);
				tmp_flipping = fb->RequiresFlipping();
			}
			for (size_t i = 0; i < sum_lums_.size(); ++ i)
			{
				FrameBufferPtr fb = rf.MakeFrameBuffer();
				fb->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*lum_texs_[i + 1], 0, 0));
				sum_lums_[i]->Source(lum_texs_[i], tmp_flipping);
				sum_lums_[i]->Destinate(fb);
				tmp_flipping = fb->RequiresFlipping();
			}
		}

		{
			adapted_lum_->Source(lum_texs_[sum_lums_.size()], tmp_flipping);
		}

		{
			tone_mapping_->Source(src_texture_, flipping);
		}
	}

	void HDRPostProcess::Destinate(FrameBufferPtr const & fb)
	{
		tone_mapping_->Destinate(fb);
	}

	void HDRPostProcess::Apply()
	{
		// 降采样
		downsampler_->Apply();
		// Blur
		blur_->Apply();

		// 降采样4x4 log
		sum_lums_1st_->Apply();
		for (size_t i = 0; i < sum_lums_.size(); ++ i)
		{
			// 降采样4x4
			sum_lums_[i]->Apply();
		}

		adapted_lum_->Apply();

		{
			// Tone mapping
			checked_pointer_cast<ToneMappingPostProcess>(tone_mapping_)->SetTexture(
				checked_pointer_cast<AdaptedLumPostProcess>(adapted_lum_)->AdaptedLum(), blur_tex_);
			tone_mapping_->Apply();
		}
	}
}
