/*
 Copyright (C) 1999-2006 Id Software, Inc. and contributors.
 For a list of contributors, see the accompanying CONTRIBUTORS file.

 This file is part of GtkRadiant.

 GtkRadiant is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 GtkRadiant is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with GtkRadiant; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "textures.h"

#include "debugging/debugging.h"

#include "itextures.h"
#include "igl.h"
#include "preferencesystem.h"
#include "render/qgl.h"
#include "radiant_i18n.h"

#include "AutoPtr.h"
#include "texturelib.h"
#include "container/hashfunc.h"
#include "container/cache.h"
#include "generic/callback.h"
#include "stringio.h"

#include "image.h"
#include "texmanip.h"
#include "settings/preferences.h"

enum ETexturesMode
{
	eTextures_NEAREST = 0,
	eTextures_NEAREST_MIPMAP_NEAREST = 1,
	eTextures_NEAREST_MIPMAP_LINEAR = 2,
	eTextures_LINEAR = 3,
	eTextures_LINEAR_MIPMAP_NEAREST = 4,
	eTextures_LINEAR_MIPMAP_LINEAR = 5,
	eTextures_MAX_ANISOTROPY = 6
};

enum TextureCompressionFormat
{
	TEXTURECOMPRESSION_NONE = 0,
	TEXTURECOMPRESSION_RGBA = 1,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT1 = 2,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT3 = 3,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT5 = 4
};

struct texture_globals_t
{
		// RIANT
		// texture compression format
		TextureCompressionFormat m_nTextureCompressionFormat;

		float fGamma;

		bool bTextureCompressionSupported; // is texture compression supported by hardware?
		GLint texture_components;

		// temporary values that should be initialised only once at run-time
		bool m_bOpenGLCompressionSupported;
		bool m_bS3CompressionSupported;

		texture_globals_t (GLint components) :
			m_nTextureCompressionFormat(TEXTURECOMPRESSION_NONE), fGamma(1.0f), bTextureCompressionSupported(false),
					texture_components(components), m_bOpenGLCompressionSupported(false), m_bS3CompressionSupported(
							false)
		{
		}
};

static texture_globals_t g_texture_globals(GL_RGBA);

static void SetTexParameters (ETexturesMode mode)
{
	const int maxAniso = QGL_maxTextureAnisotropy();
	if (maxAniso > 1)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
	else if (mode == eTextures_MAX_ANISOTROPY)
		mode = eTextures_LINEAR_MIPMAP_LINEAR;

	switch (mode) {
	case eTextures_NEAREST:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	case eTextures_NEAREST_MIPMAP_NEAREST:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	case eTextures_NEAREST_MIPMAP_LINEAR:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	case eTextures_LINEAR:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case eTextures_LINEAR_MIPMAP_NEAREST:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case eTextures_LINEAR_MIPMAP_LINEAR:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case eTextures_MAX_ANISOTROPY:
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAniso);
		break;
	default:
		globalErrorStream() << "invalid texture mode\n";
	}
}

static ETexturesMode g_texture_mode = eTextures_LINEAR_MIPMAP_LINEAR;

static byte g_gammatable[256];
static void ResampleGamma (float fGamma)
{
	int i, inf;
	if (fGamma == 1.0) {
		for (i = 0; i < 256; i++)
			g_gammatable[i] = i;
	} else {
		for (i = 0; i < 256; i++) {
			inf = (int) (255 * pow(static_cast<double> ((i + 0.5) / 255.5), static_cast<double> (fGamma)) + 0.5);
			if (inf < 0)
				inf = 0;
			if (inf > 255)
				inf = 255;
			g_gammatable[i] = inf;
		}
	}
}

static inline const int& min_int (const int& left, const int& right)
{
	return std::min(left, right);
}

static int max_tex_size = 0;
static const int max_texture_quality = 3;
static LatchedInt g_Textures_textureQuality(3, _("Texture Quality"));

/**
 * @brief This function does the actual processing of raw RGBA data into a GL texture.
 * @note It will also resample to power-of-two dimensions, generate the mipmaps and adjust gamma.
 */
void LoadTextureRGBA (qtexture_t* q, unsigned char* pPixels, int nWidth, int nHeight)
{
	static float fGamma = -1;
	float total[3];
	byte *outpixels = 0;
	int nCount = nWidth * nHeight;

	if (fGamma != g_texture_globals.fGamma) {
		fGamma = g_texture_globals.fGamma;
		ResampleGamma(fGamma);
	}

	q->width = nWidth;
	q->height = nHeight;

	total[0] = total[1] = total[2] = 0.0f;

	// resample texture gamma according to user settings
	for (int i = 0; i < (nCount * 4); i += 4) {
		for (int j = 0; j < 3; j++) {
			total[j] += (pPixels + i)[j];
			byte b = (pPixels + i)[j];
			(pPixels + i)[j] = g_gammatable[b];
		}
	}

	q->color[0] = total[0] / (nCount * 255);
	q->color[1] = total[1] / (nCount * 255);
	q->color[2] = total[2] / (nCount * 255);

	glGenTextures(1, &q->texture_number);

	glBindTexture(GL_TEXTURE_2D, q->texture_number);

	SetTexParameters(g_texture_mode);

	int gl_width = 1;
	while (gl_width < nWidth)
		gl_width <<= 1;

	int gl_height = 1;
	while (gl_height < nHeight)
		gl_height <<= 1;

	bool resampled = false;
	if (!(gl_width == nWidth && gl_height == nHeight)) {
		resampled = true;
		outpixels = (byte *) malloc(gl_width * gl_height * 4);
		R_ResampleTexture(pPixels, nWidth, nHeight, outpixels, gl_width, gl_height, 4);
	} else {
		outpixels = pPixels;
	}

	int quality_reduction = max_texture_quality - g_Textures_textureQuality.m_value;
	int target_width = min_int(gl_width >> quality_reduction, max_tex_size);
	int target_height = min_int(gl_height >> quality_reduction, max_tex_size);

	while (gl_width > target_width || gl_height > target_height) {
		GL_MipReduce(outpixels, outpixels, gl_width, gl_height, target_width, target_height);

		if (gl_width > target_width)
			gl_width >>= 1;
		if (gl_height > target_height)
			gl_height >>= 1;
	}

	int mip = 0;
	glTexImage2D(GL_TEXTURE_2D, mip++, g_texture_globals.texture_components, gl_width, gl_height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, outpixels);
	while (gl_width > 1 || gl_height > 1) {
		GL_MipReduce(outpixels, outpixels, gl_width, gl_height, 1, 1);

		if (gl_width > 1)
			gl_width >>= 1;
		if (gl_height > 1)
			gl_height >>= 1;

		glTexImage2D(GL_TEXTURE_2D, mip++, g_texture_globals.texture_components, gl_width, gl_height, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, outpixels);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	if (resampled)
		free(outpixels);
}

typedef std::pair<LoadImageCallback, std::string> TextureKey;

void qtexture_realise (qtexture_t& texture, const TextureKey& key)
{
	texture.texture_number = 0;
	/* skip empty names and normalmaps */
	if (!key.second.empty() && !strstr(key.second.c_str(), "_nm")) {
		AutoPtr<Image> image(key.first.loadImage(key.second.c_str()));
		if (image) {
			LoadTextureRGBA(&texture, image->getRGBAPixels(), image->getWidth(), image->getHeight());
			texture.surfaceFlags = image->getSurfaceFlags();
			texture.contentFlags = image->getContentFlags();
			texture.value = image->getValue();
			g_message("Loaded Texture: \"%s\"\n", key.second.c_str());
		} else {
			g_warning("Texture load failed: \"%s\"\n", key.second.c_str());
		}
	}
}

void qtexture_unrealise (qtexture_t& texture)
{
	if (GlobalOpenGL().contextValid && texture.texture_number != 0) {
		glDeleteTextures(1, &texture.texture_number);
	}
}

class TextureKeyEqualNoCase
{
	public:
		bool operator() (const TextureKey& key, const TextureKey& other) const
		{
			return key.first == other.first && string_equal_nocase(key.second.c_str(), other.second.c_str());
		}
};

class TextureKeyHashNoCase
{
	public:
		typedef hash_t hash_type;
		hash_t operator() (const TextureKey& key) const
		{
			return hash_combine(string_hash_nocase(key.second.c_str()), pod_hash(key.first));
		}
};

#define DEBUG_TEXTURES 0

class TexturesMap: public TexturesCache
{
		class TextureConstructor
		{
				TexturesMap* m_cache;
			public:
				explicit TextureConstructor (TexturesMap* cache) :
					m_cache(cache)
				{
				}
				qtexture_t* construct (const TextureKey& key)
				{
					qtexture_t* texture = new qtexture_t(key.first, key.second);
					if (m_cache->realised()) {
						qtexture_realise(*texture, key);
					}
					return texture;
				}
				void destroy (qtexture_t* texture)
				{
					if (m_cache->realised()) {
						qtexture_unrealise(*texture);
					}
					delete texture;
				}
		};

		typedef HashedCache<TextureKey, qtexture_t, TextureKeyHashNoCase, TextureKeyEqualNoCase, TextureConstructor>
				qtextures_t;
		qtextures_t m_qtextures;
		TexturesCacheObserver* m_observer;
		std::size_t m_unrealised;

	public:
		TexturesMap () :
			m_qtextures(TextureConstructor(this)), m_observer(0), m_unrealised(1)
		{
		}
		typedef qtextures_t::iterator iterator;

		iterator begin ()
		{
			return m_qtextures.begin();
		}
		iterator end ()
		{
			return m_qtextures.end();
		}

		LoadImageCallback defaultLoader () const
		{
			return LoadImageCallback(0, QERApp_LoadImage);
		}
		Image* loadImage (const std::string& name)
		{
			return defaultLoader().loadImage(name);
		}
		qtexture_t* capture (const std::string& name)
		{
			return capture(defaultLoader(), name);
		}
		qtexture_t* capture (const LoadImageCallback& loader, const std::string& name)
		{
			g_debug("textures capture: '%s'\n", name.c_str());
			return m_qtextures.capture(TextureKey(loader, name)).get();
		}
		void release (qtexture_t* texture)
		{
			g_debug("textures release: '%s'\n", texture->name.c_str());
			m_qtextures.release(TextureKey(texture->load, texture->name));
		}
		void attach (TexturesCacheObserver& observer)
		{
			ASSERT_MESSAGE(m_observer == 0, "TexturesMap::attach: cannot attach observer");
			m_observer = &observer;
		}
		void detach (TexturesCacheObserver& observer)
		{
			ASSERT_MESSAGE(m_observer == &observer, "TexturesMap::detach: cannot detach observer");
			m_observer = 0;
		}
		void realise ()
		{
			if (--m_unrealised == 0) {
				g_texture_globals.bTextureCompressionSupported = false;

				if (GlobalOpenGL().ARB_texture_compression()) {
					g_texture_globals.bTextureCompressionSupported = true;
					g_texture_globals.m_bOpenGLCompressionSupported = true;
				}

				if (GlobalOpenGL().EXT_texture_compression_s3tc()) {
					g_texture_globals.bTextureCompressionSupported = true;
					g_texture_globals.m_bS3CompressionSupported = true;
				}

				glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
				if (max_tex_size == 0) {
					max_tex_size = 1024;
				}

				for (qtextures_t::iterator i = m_qtextures.begin(); i != m_qtextures.end(); ++i) {
					if (!(*i).value.empty()) {
						qtexture_realise(*(*i).value, (*i).key);
					}
				}
				if (m_observer != 0) {
					m_observer->realise();
				}
			}
		}
		void unrealise ()
		{
			if (++m_unrealised == 1) {
				if (m_observer != 0) {
					m_observer->unrealise();
				}
				for (qtextures_t::iterator i = m_qtextures.begin(); i != m_qtextures.end(); ++i) {
					if (!(*i).value.empty()) {
						qtexture_unrealise(*(*i).value);
					}
				}
			}
		}
		bool realised ()
		{
			return m_unrealised == 0;
		}
};

TexturesMap* g_texturesmap;

TexturesCache& GetTexturesCache ()
{
	return *g_texturesmap;
}

void Textures_Realise ()
{
	g_texturesmap->realise();
}

void Textures_Unrealise ()
{
	g_texturesmap->unrealise();
}

Callback g_texturesModeChangedNotify;

void Textures_setModeChangedNotify (const Callback& notify)
{
	g_texturesModeChangedNotify = notify;
}

void Textures_ModeChanged ()
{
	if (g_texturesmap->realised()) {
		SetTexParameters(g_texture_mode);

		for (TexturesMap::iterator i = g_texturesmap->begin(); i != g_texturesmap->end(); ++i) {
			glBindTexture(GL_TEXTURE_2D, (*i).value->texture_number);
			SetTexParameters(g_texture_mode);
		}

		glBindTexture(GL_TEXTURE_2D, 0);
	}
	g_texturesModeChangedNotify();
}

void Textures_SetMode (ETexturesMode mode)
{
	if (g_texture_mode != mode) {
		g_texture_mode = mode;

		Textures_ModeChanged();
	}
}

void Textures_setTextureComponents (GLint texture_components)
{
	if (g_texture_globals.texture_components != texture_components) {
		Textures_Unrealise();
		g_texture_globals.texture_components = texture_components;
		Textures_Realise();
	}
}

void Textures_UpdateTextureCompressionFormat ()
{
	GLint texture_components = GL_RGBA;

	if (!g_texturesmap->realised()) {
		texture_components = g_texture_globals.m_nTextureCompressionFormat;
		if (texture_components == TEXTURECOMPRESSION_NONE)
			texture_components = GL_RGBA;
	} else {
		if (g_texture_globals.bTextureCompressionSupported) {
			if (g_texture_globals.m_nTextureCompressionFormat != TEXTURECOMPRESSION_NONE
					&& g_texture_globals.m_nTextureCompressionFormat != TEXTURECOMPRESSION_RGBA
					&& !g_texture_globals.m_bS3CompressionSupported) {
				g_message(
						"OpenGL extension GL_EXT_texture_compression_s3tc not supported by current graphics drivers\n");
				g_texture_globals.m_nTextureCompressionFormat = TEXTURECOMPRESSION_RGBA; // if this is not supported either, see below
			}
			if (g_texture_globals.m_nTextureCompressionFormat == TEXTURECOMPRESSION_RGBA
					&& !g_texture_globals.m_bOpenGLCompressionSupported) {
				g_message("OpenGL extension GL_ARB_texture_compression not supported by current graphics drivers\n");
				g_texture_globals.m_nTextureCompressionFormat = TEXTURECOMPRESSION_NONE;
			}

			switch (g_texture_globals.m_nTextureCompressionFormat) {
			case (TEXTURECOMPRESSION_NONE): {
				texture_components = GL_RGBA;
				break;
			}
			case (TEXTURECOMPRESSION_RGBA): {
				texture_components = GL_COMPRESSED_RGBA_ARB;
				break;
			}
			case (TEXTURECOMPRESSION_RGBA_S3TC_DXT1): {
				texture_components = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
				break;
			}
			case (TEXTURECOMPRESSION_RGBA_S3TC_DXT3): {
				texture_components = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
				break;
			}
			case (TEXTURECOMPRESSION_RGBA_S3TC_DXT5): {
				texture_components = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
				break;
			}
			}
		} else {
			texture_components = GL_RGBA;
			g_texture_globals.m_nTextureCompressionFormat = TEXTURECOMPRESSION_NONE;
		}
	}

	Textures_setTextureComponents(texture_components);
}

void TextureCompressionImport (TextureCompressionFormat& self, int value)
{
	if (!g_texture_globals.m_bOpenGLCompressionSupported && g_texture_globals.m_bS3CompressionSupported && value >= 1) {
		++value;
	}
	switch (value) {
	case 0:
		self = TEXTURECOMPRESSION_NONE;
		break;
	case 1:
		self = TEXTURECOMPRESSION_RGBA;
		break;
	case 2:
		self = TEXTURECOMPRESSION_RGBA_S3TC_DXT1;
		break;
	case 3:
		self = TEXTURECOMPRESSION_RGBA_S3TC_DXT3;
		break;
	case 4:
		self = TEXTURECOMPRESSION_RGBA_S3TC_DXT5;
		break;
	}
	Textures_UpdateTextureCompressionFormat();
}
typedef ReferenceCaller1<TextureCompressionFormat, int, TextureCompressionImport> TextureCompressionImportCaller;

void TextureGammaImport (float& self, float value)
{
	if (self != value) {
		Textures_Unrealise();
		self = value;
		Textures_Realise();
	}
}
typedef ReferenceCaller1<float, float, TextureGammaImport> TextureGammaImportCaller;

void TextureModeImport (ETexturesMode& self, int value)
{
	switch (value) {
	case 0:
		Textures_SetMode(eTextures_NEAREST);
		break;
	case 1:
		Textures_SetMode(eTextures_NEAREST_MIPMAP_NEAREST);
		break;
	case 2:
		Textures_SetMode(eTextures_LINEAR);
		break;
	case 3:
		Textures_SetMode(eTextures_NEAREST_MIPMAP_LINEAR);
		break;
	case 4:
		Textures_SetMode(eTextures_LINEAR_MIPMAP_NEAREST);
		break;
	case 5:
		Textures_SetMode(eTextures_LINEAR_MIPMAP_LINEAR);
		break;
	case 6:
		Textures_SetMode(eTextures_MAX_ANISOTROPY);
	}
}
typedef ReferenceCaller1<ETexturesMode, int, TextureModeImport> TextureModeImportCaller;

void TextureModeExport (ETexturesMode& self, const IntImportCallback& importer)
{
	switch (self) {
	case eTextures_NEAREST:
		importer(0);
		break;
	case eTextures_NEAREST_MIPMAP_NEAREST:
		importer(1);
		break;
	case eTextures_LINEAR:
		importer(2);
		break;
	case eTextures_NEAREST_MIPMAP_LINEAR:
		importer(3);
		break;
	case eTextures_LINEAR_MIPMAP_NEAREST:
		importer(4);
		break;
	case eTextures_LINEAR_MIPMAP_LINEAR:
		importer(5);
		break;
	case eTextures_MAX_ANISOTROPY:
		importer(6);
		break;
	default:
		importer(4);
	}
}
typedef ReferenceCaller1<ETexturesMode, const IntImportCallback&, TextureModeExport> TextureModeExportCaller;

void Textures_constructPreferences (PreferencesPage& page)
{
	{
		const char* percentages[] = { N_("12.5%"), N_("25%"), N_("50%"), N_("100%") };
		page.appendRadio(_("Texture Quality"), STRING_ARRAY_RANGE(percentages), LatchedIntImportCaller(
				g_Textures_textureQuality), IntExportCaller(g_Textures_textureQuality.m_latched));
	}
	page.appendSpinner(_("Texture Gamma"), 1.0, 0.0, 1.0, FloatImportCallback(TextureGammaImportCaller(
			g_texture_globals.fGamma)), FloatExportCallback(FloatExportCaller(g_texture_globals.fGamma)));
	{
		const char* texture_mode[] = {
				N_("Nearest"),
				N_("Nearest Mipmap"),
				N_("Linear"),
				N_("Bilinear"),
				N_("Bilinear Mipmap"),
				N_("Trilinear"),
				N_("Anisotropy") };
		page.appendCombo(_("Texture Render Mode"), STRING_ARRAY_RANGE(texture_mode), IntImportCallback(
				TextureModeImportCaller(g_texture_mode)), IntExportCallback(TextureModeExportCaller(g_texture_mode)));
	}
	{
		const char* compression_none[] = { N_("None") };
		const char* compression_opengl[] = { N_("None"), N_("OpenGL ARB") };
		const char* compression_s3tc[] = { N_("None"), N_("S3TC DXT1"), N_("S3TC DXT3"), N_("S3TC DXT5") };
		const char* compression_opengl_s3tc[] = {
				N_("None"),
				N_("OpenGL ARB"),
				N_("S3TC DXT1"),
				N_("S3TC DXT3"),
				N_("S3TC DXT5") };
		StringArrayRange
				compression(
						(g_texture_globals.m_bOpenGLCompressionSupported) ? (g_texture_globals.m_bS3CompressionSupported) ? STRING_ARRAY_RANGE(compression_opengl_s3tc)
								: STRING_ARRAY_RANGE(compression_opengl)
								: (g_texture_globals.m_bS3CompressionSupported) ? STRING_ARRAY_RANGE(compression_s3tc)
										: STRING_ARRAY_RANGE(compression_none));
		page.appendCombo(_("Hardware Texture Compression"), compression, TextureCompressionImportCaller(
				g_texture_globals.m_nTextureCompressionFormat), IntExportCaller(
				reinterpret_cast<int&> (g_texture_globals.m_nTextureCompressionFormat)));
	}
}
void Textures_constructPage (PreferenceGroup& group)
{
	PreferencesPage page(group.createPage(_("Textures"), _("Texture Settings")));
	Textures_constructPreferences(page);
}
void Textures_registerPreferencesPage ()
{
	PreferencesDialog_addSettingsPage(FreeCaller1<PreferenceGroup&, Textures_constructPage> ());
}

void TextureCompression_importString (const char* string)
{
	g_texture_globals.m_nTextureCompressionFormat = static_cast<TextureCompressionFormat> (atoi(string));
	Textures_UpdateTextureCompressionFormat();
}
typedef FreeCaller1<const char*, TextureCompression_importString> TextureCompressionImportStringCaller;

void Textures_Construct ()
{
	g_texturesmap = new TexturesMap;

	GlobalPreferenceSystem().registerPreference("TextureCompressionFormat", TextureCompressionImportStringCaller(),
			IntExportStringCaller(reinterpret_cast<int&> (g_texture_globals.m_nTextureCompressionFormat)));
	GlobalPreferenceSystem().registerPreference("TextureFiltering", IntImportStringCaller(
			reinterpret_cast<int&> (g_texture_mode)), IntExportStringCaller(reinterpret_cast<int&> (g_texture_mode)));
	GlobalPreferenceSystem().registerPreference("TextureQuality", IntImportStringCaller(
			g_Textures_textureQuality.m_latched), IntExportStringCaller(g_Textures_textureQuality.m_latched));
	GlobalPreferenceSystem().registerPreference("SI_Gamma", FloatImportStringCaller(g_texture_globals.fGamma),
			FloatExportStringCaller(g_texture_globals.fGamma));

	g_Textures_textureQuality.useLatched();

	Textures_registerPreferencesPage();

	Textures_ModeChanged();
}
void Textures_Destroy ()
{
	delete g_texturesmap;
}

#include "modulesystem/modulesmap.h"
#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class TexturesDependencies: public GlobalRadiantModuleRef,
		public GlobalOpenGLModuleRef,
		public GlobalPreferenceSystemModuleRef
{
		ImageModulesRef m_image_modules;
	public:
		TexturesDependencies () :
			m_image_modules(GlobalRadiant().getRequiredGameDescriptionKeyValue("texturetypes"))
		{
		}
		ImageModules& getImageModules ()
		{
			return m_image_modules.get();
		}
};

class TexturesAPI
{
		TexturesCache* m_textures;
	public:
		typedef TexturesCache Type;
		STRING_CONSTANT(Name, "*");

		TexturesAPI ()
		{
			Textures_Construct();

			m_textures = &GetTexturesCache();
		}
		~TexturesAPI ()
		{
			Textures_Destroy();
		}
		TexturesCache* getTable ()
		{
			return m_textures;
		}
};

typedef SingletonModule<TexturesAPI, TexturesDependencies> TexturesModule;
typedef Static<TexturesModule> StaticTexturesModule;
StaticRegisterModule staticRegisterTextures(StaticTexturesModule::instance());

ImageModules& Textures_getImageModules ()
{
	return StaticTexturesModule::instance().getDependencies().getImageModules();
}
