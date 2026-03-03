#include <stdint.h>
#include <stdlib.h>

#include <cstring>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include <artery-font/std-artery-font.h>
#include <artery-font/serialization.h>

#include "FontGeometry.h"
#include "ImmediateAtlasGenerator.h"
#include "TightAtlasPacker.h"
#include "BitmapAtlasStorage.h"
#include "glyph-generators.h"
#include "Charset.h"

namespace {

using msdf_atlas::BitmapAtlasStorage;
using msdf_atlas::Charset;
using msdf_atlas::DimensionsConstraint;
using msdf_atlas::FontGeometry;
using msdf_atlas::GeneratorAttributes;
using msdf_atlas::GlyphGeometry;
using msdf_atlas::ImmediateAtlasGenerator;
using msdf_atlas::TightAtlasPacker;
using msdf_atlas::byte;

struct ArenaString {
    uint8_t *ptr = nullptr;
    uint32_t len = 0;
};

static ArenaString alloc_copy(const std::string &s) {
    ArenaString out;
    if (s.empty()) return out;
    out.len = static_cast<uint32_t>(s.size());
    out.ptr = static_cast<uint8_t *>(malloc(out.len));
    if (out.ptr) {
        memcpy(out.ptr, s.data(), out.len);
    } else {
        out.len = 0;
    }
    return out;
}

template <int N>
struct AtlasBytes {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

template <int N, msdf_atlas::GeneratorFunction<float, N> GEN_FN>
static bool generate_atlas_bytes(
    const std::vector<GlyphGeometry> &glyphs,
    int width,
    int height,
    const GeneratorAttributes &attribs,
    int thread_count,
    AtlasBytes<N> &out
) {
    ImmediateAtlasGenerator<float, N, GEN_FN, BitmapAtlasStorage<byte, N> > generator(width, height);
    generator.setAttributes(attribs);
    generator.setThreadCount(thread_count);
    generator.generate(glyphs.data(), static_cast<int>(glyphs.size()));

    msdfgen::BitmapConstRef<byte, N> bitmap(generator.atlasStorage());
    const uint32_t size = static_cast<uint32_t>(bitmap.width * bitmap.height * N);
    out.width = bitmap.width;
    out.height = bitmap.height;
    out.data.resize(size);
    if (size > 0) {
        memcpy(out.data.data(), bitmap.pixels, size);
    }
    return true;
}

static const char *image_type_name(uint32_t atlas_type) {
    switch (atlas_type) {
        case 0: return "sdf";
        case 1: return "psdf";
        case 2: return "msdf";
        case 3: return "mtsdf";
        default: return "mtsdf";
    }
}

static artery_font::ImageType to_artery_image_type(uint32_t atlas_type) {
    switch (atlas_type) {
        case 0: return artery_font::IMAGE_SDF;
        case 1: return artery_font::IMAGE_PSDF;
        case 2: return artery_font::IMAGE_MSDF;
        case 3: return artery_font::IMAGE_MTSDF;
        default: return artery_font::IMAGE_MTSDF;
    }
}

static artery_font::CodepointType to_artery_codepoint_type(msdf_atlas::GlyphIdentifierType id_type) {
    switch (id_type) {
        case msdf_atlas::GlyphIdentifierType::GLYPH_INDEX:
            return artery_font::CP_INDEXED;
        case msdf_atlas::GlyphIdentifierType::UNICODE_CODEPOINT:
            return artery_font::CP_UNICODE;
    }
    return artery_font::CP_UNSPECIFIED;
}

static int write_vector_callback(const void *src, int length, void *user_data) {
    if (!src || length < 0 || !user_data) return 0;
    auto *out = reinterpret_cast<std::vector<uint8_t> *>(user_data);
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(src);
    out->insert(out->end(), bytes, bytes+length);
    return length;
}

static bool build_artery_font_binary(
    const FontGeometry &font,
    const std::vector<uint8_t> &atlas_data,
    int atlas_w,
    int atlas_h,
    int channels,
    uint32_t atlas_type,
    double em_size,
    double px_range,
    std::vector<uint8_t> &out
) {
    artery_font::StdArteryFont<float> arfont = { };
    arfont.metadataFormat = artery_font::METADATA_NONE;

    arfont.variants = artery_font::StdList<artery_font::StdArteryFont<float>::Variant>(1);
    auto &variant = arfont.variants[0] = artery_font::StdArteryFont<float>::Variant();

    const msdfgen::FontMetrics &metrics = font.getMetrics();
    msdf_atlas::GlyphIdentifierType id_type = font.getPreferredIdentifierType();
    variant.codepointType = to_artery_codepoint_type(id_type);
    variant.imageType = to_artery_image_type(atlas_type);

    variant.metrics.fontSize = float(em_size*metrics.emSize);
    variant.metrics.distanceRange = float(px_range);
    variant.metrics.distanceRangeMiddle = 0.f;
    variant.metrics.emSize = float(metrics.emSize);
    variant.metrics.ascender = float(metrics.ascenderY);
    variant.metrics.descender = float(metrics.descenderY);
    variant.metrics.lineHeight = float(metrics.lineHeight);
    variant.metrics.underlineY = float(metrics.underlineY);
    variant.metrics.underlineThickness = float(metrics.underlineThickness);

    const char *name = font.getName();
    if (name) {
        (std::string &) variant.name = name;
    }

    const auto &glyphs = font.getGlyphs();
    variant.glyphs = artery_font::StdList<artery_font::Glyph<float> >(glyphs.size());
    int gi = 0;
    for (const GlyphGeometry &glyph_geom : glyphs) {
        artery_font::Glyph<float> &glyph = variant.glyphs[gi++];
        glyph.codepoint = glyph_geom.getIdentifier(id_type);
        glyph.image = 0;

        double l, b, r, t;
        glyph_geom.getQuadPlaneBounds(l, b, r, t);
        glyph.planeBounds.l = float(l);
        glyph.planeBounds.b = float(b);
        glyph.planeBounds.r = float(r);
        glyph.planeBounds.t = float(t);

        glyph_geom.getQuadAtlasBounds(l, b, r, t);
        glyph.imageBounds.l = float(l);
        glyph.imageBounds.b = float(b);
        glyph.imageBounds.r = float(r);
        glyph.imageBounds.t = float(t);

        glyph.advance.h = float(glyph_geom.getAdvance());
        glyph.advance.v = 0.f;
    }

    for (const std::pair<std::pair<int, int>, double> &elem : font.getKerning()) {
        artery_font::KernPair<float> kern = { };
        if (id_type == msdf_atlas::GlyphIdentifierType::GLYPH_INDEX) {
            kern.codepoint1 = elem.first.first;
            kern.codepoint2 = elem.first.second;
            kern.advance.h = float(elem.second);
            ((std::vector<artery_font::KernPair<float> > &) variant.kernPairs).push_back((artery_font::KernPair<float> &&) kern);
        } else {
            const GlyphGeometry *g1 = font.getGlyph(msdfgen::GlyphIndex(elem.first.first));
            const GlyphGeometry *g2 = font.getGlyph(msdfgen::GlyphIndex(elem.first.second));
            if (g1 && g2 && g1->getCodepoint() && g2->getCodepoint()) {
                kern.codepoint1 = g1->getCodepoint();
                kern.codepoint2 = g2->getCodepoint();
                kern.advance.h = float(elem.second);
                ((std::vector<artery_font::KernPair<float> > &) variant.kernPairs).push_back((artery_font::KernPair<float> &&) kern);
            }
        }
    }

    arfont.images = artery_font::StdList<artery_font::StdArteryFont<float>::Image>(1);
    auto &image = arfont.images[0] = artery_font::StdArteryFont<float>::Image();
    image.width = atlas_w;
    image.height = atlas_h;
    image.channels = channels;
    image.imageType = to_artery_image_type(atlas_type);
    image.encoding = artery_font::IMAGE_RAW_BINARY;
    image.pixelFormat = artery_font::PIXEL_UNSIGNED8;
    image.rawBinaryFormat.rowLength = channels*atlas_w;
    image.rawBinaryFormat.orientation = artery_font::ORIENTATION_BOTTOM_UP;
    image.data = artery_font::StdByteArray(atlas_data.size());
    memcpy((uint8_t *) image.data, atlas_data.data(), atlas_data.size());

    out.clear();
    out.reserve(atlas_data.size()/2);
    return artery_font::encode<write_vector_callback>(arfont, &out);
}

static void append_json_number(std::ostringstream &ss, double n) {
    ss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    ss.precision(17);
    ss << n;
}

static std::string build_json(
    const FontGeometry &font,
    uint32_t atlas_type,
    int atlas_w,
    int atlas_h,
    double em_size,
    double px_range,
    bool y_top
) {
    std::ostringstream ss;
    const auto &m = font.getMetrics();

    ss << "{";
    ss << "\"atlas\":{";
    ss << "\"type\":\"" << image_type_name(atlas_type) << "\",";
    ss << "\"distanceRange\":";
    append_json_number(ss, px_range);
    ss << ",\"distanceRangeMiddle\":0,";
    ss << "\"size\":";
    append_json_number(ss, em_size);
    ss << ",\"width\":" << atlas_w << ",\"height\":" << atlas_h;
    ss << ",\"yOrigin\":\"" << (y_top ? "top" : "bottom") << "\"},";

    const double y_factor = y_top ? -1.0 : 1.0;
    ss << "\"metrics\":{";
    ss << "\"emSize\":";
    append_json_number(ss, m.emSize);
    ss << ",\"lineHeight\":";
    append_json_number(ss, m.lineHeight);
    ss << ",\"ascender\":";
    append_json_number(ss, y_factor * m.ascenderY);
    ss << ",\"descender\":";
    append_json_number(ss, y_factor * m.descenderY);
    ss << ",\"underlineY\":";
    append_json_number(ss, y_factor * m.underlineY);
    ss << ",\"underlineThickness\":";
    append_json_number(ss, m.underlineThickness);
    ss << "},";

    ss << "\"glyphs\":[";
    bool first = true;
    for (const auto &glyph : font.getGlyphs()) {
        if (!first) ss << ",";
        first = false;
        ss << "{";
        ss << "\"unicode\":" << glyph.getCodepoint() << ",\"advance\":";
        append_json_number(ss, glyph.getAdvance());

        double l = 0, b = 0, r = 0, t = 0;
        glyph.getQuadPlaneBounds(l, b, r, t);
        if (l || b || r || t) {
            ss << ",\"planeBounds\":{";
            if (y_top) {
                ss << "\"left\":";
                append_json_number(ss, l);
                ss << ",\"top\":";
                append_json_number(ss, -t);
                ss << ",\"right\":";
                append_json_number(ss, r);
                ss << ",\"bottom\":";
                append_json_number(ss, -b);
            } else {
                ss << "\"left\":";
                append_json_number(ss, l);
                ss << ",\"bottom\":";
                append_json_number(ss, b);
                ss << ",\"right\":";
                append_json_number(ss, r);
                ss << ",\"top\":";
                append_json_number(ss, t);
            }
            ss << "}";
        }

        glyph.getQuadAtlasBounds(l, b, r, t);
        if (l || b || r || t) {
            ss << ",\"atlasBounds\":{";
            if (y_top) {
                ss << "\"left\":";
                append_json_number(ss, l);
                ss << ",\"top\":";
                append_json_number(ss, atlas_h - t);
                ss << ",\"right\":";
                append_json_number(ss, r);
                ss << ",\"bottom\":";
                append_json_number(ss, atlas_h - b);
            } else {
                ss << "\"left\":";
                append_json_number(ss, l);
                ss << ",\"bottom\":";
                append_json_number(ss, b);
                ss << ",\"right\":";
                append_json_number(ss, r);
                ss << ",\"top\":";
                append_json_number(ss, t);
            }
            ss << "}";
        }

        ss << "}";
    }
    ss << "]}";
    return ss.str();
}

} // namespace

extern "C" {

struct MagConfig {
    uint32_t atlas_type;
    float em_size;
    float px_range;
    float miter_limit;
    uint32_t width;
    uint32_t height;
    uint32_t edge_coloring;
    uint32_t expensive_coloring;
    uint32_t scanline_pass;
    uint32_t preprocess;
    uint32_t y_origin_top;
    uint32_t thread_count;
    uint32_t export_artery;
};

struct MagResult {
    uint32_t ok;
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t atlas_channels;
    uint32_t atlas_ptr;
    uint32_t atlas_len;
    uint32_t json_ptr;
    uint32_t json_len;
    uint32_t artery_ptr;
    uint32_t artery_len;
    uint32_t error_ptr;
    uint32_t error_len;
};

static void set_error(MagResult *out, const char *msg) {
    if (!out || !msg) return;
    const uint32_t len = static_cast<uint32_t>(strlen(msg));
    uint8_t *ptr = nullptr;
    if (len > 0) {
        ptr = static_cast<uint8_t *>(malloc(len));
        if (ptr) memcpy(ptr, msg, len);
    }
    out->ok = 0;
    out->error_ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
    out->error_len = ptr ? len : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t mag_alloc(uint32_t size) {
    if (size == 0) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(malloc(size)));
}

EMSCRIPTEN_KEEPALIVE void mag_free(uint32_t ptr) {
    if (ptr != 0) {
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(ptr)));
    }
}

EMSCRIPTEN_KEEPALIVE void mag_release_result(struct MagResult *result) {
    if (!result) return;
    if (result->atlas_ptr) {
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(result->atlas_ptr)));
    }
    if (result->json_ptr) {
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(result->json_ptr)));
    }
    if (result->artery_ptr) {
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(result->artery_ptr)));
    }
    if (result->error_ptr) {
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(result->error_ptr)));
    }
    memset(result, 0, sizeof(MagResult));
}

EMSCRIPTEN_KEEPALIVE int mag_generate(
    const uint8_t *font_data,
    uint32_t font_len,
    const uint32_t *codepoints,
    uint32_t codepoint_count,
    const struct MagConfig *cfg,
    struct MagResult *out
) {
    if (!out) return 0;
    memset(out, 0, sizeof(MagResult));

    if (!font_data || font_len == 0 || !cfg) {
        set_error(out, "Invalid input pointers or empty font data");
        return 0;
    }

    msdfgen::FreetypeHandle *ft = msdfgen::initializeFreetype();
    if (!ft) {
        set_error(out, "Failed to initialize FreeType");
        return 0;
    }

    msdfgen::FontHandle *font = msdfgen::loadFontData(ft, font_data, static_cast<int>(font_len));
    if (!font) {
        msdfgen::deinitializeFreetype(ft);
        set_error(out, "Failed to load font bytes");
        return 0;
    }

    int ok = 0;
    try {
        std::vector<GlyphGeometry> glyphs;
        FontGeometry font_geometry(&glyphs);

        Charset charset;
        if (codepoint_count > 0 && codepoints) {
            for (uint32_t i = 0; i < codepoint_count; i++) {
                charset.add(codepoints[i]);
            }
        } else {
            for (uint32_t cp = 0x20; cp <= 0x7E; cp++) {
                charset.add(cp);
            }
        }

        int glyphs_loaded = font_geometry.loadCharset(font, 1.0, charset, cfg->preprocess != 0, false);
        if (glyphs_loaded <= 0) {
            set_error(out, "No glyphs loaded from font");
            throw 1;
        }

        void (*coloring_fn)(msdfgen::Shape &, double, unsigned long long) = &msdfgen::edgeColoringInkTrap;
        if (cfg->edge_coloring == 0) coloring_fn = &msdfgen::edgeColoringSimple;
        if (cfg->edge_coloring == 2) coloring_fn = &msdfgen::edgeColoringByDistance;

        for (GlyphGeometry &glyph : glyphs) {
            glyph.edgeColoring(coloring_fn, 3.0, 0);
        }

        const double em_size = cfg->em_size > 0 ? cfg->em_size : 48.0;
        const double px_range = cfg->px_range > 0 ? cfg->px_range : 8.0;
        const double miter_limit = cfg->miter_limit > 0 ? cfg->miter_limit : 1.0;

        TightAtlasPacker packer;
        if (cfg->width > 0 && cfg->height > 0) {
            packer.setDimensions(static_cast<int>(cfg->width), static_cast<int>(cfg->height));
        } else {
            packer.setDimensionsConstraint(DimensionsConstraint::MULTIPLE_OF_FOUR_SQUARE);
        }
        packer.setMinimumScale(em_size);
        packer.setPixelRange(px_range);
        packer.setMiterLimit(miter_limit);
        if (packer.pack(glyphs.data(), static_cast<int>(glyphs.size())) != 0) {
            set_error(out, "Atlas packing failed");
            throw 1;
        }

        int atlas_w = 0;
        int atlas_h = 0;
        packer.getDimensions(atlas_w, atlas_h);

        GeneratorAttributes attributes;
        attributes.scanlinePass = cfg->scanline_pass != 0;
        attributes.config.overlapSupport = cfg->expensive_coloring != 0;

        const int thread_count = std::max(1u, cfg->thread_count);

        std::vector<uint8_t> atlas_data;
        uint32_t channels = 0;

        if (cfg->atlas_type == 0) {
            AtlasBytes<1> bytes;
            if (!generate_atlas_bytes<1, msdf_atlas::sdfGenerator>(glyphs, atlas_w, atlas_h, attributes, thread_count, bytes)) {
                set_error(out, "SDF atlas generation failed");
                throw 1;
            }
            channels = 1;
            atlas_data.swap(bytes.data);
        } else if (cfg->atlas_type == 1) {
            AtlasBytes<1> bytes;
            if (!generate_atlas_bytes<1, msdf_atlas::psdfGenerator>(glyphs, atlas_w, atlas_h, attributes, thread_count, bytes)) {
                set_error(out, "PSDF atlas generation failed");
                throw 1;
            }
            channels = 1;
            atlas_data.swap(bytes.data);
        } else if (cfg->atlas_type == 2) {
            AtlasBytes<3> bytes;
            if (!generate_atlas_bytes<3, msdf_atlas::msdfGenerator>(glyphs, atlas_w, atlas_h, attributes, thread_count, bytes)) {
                set_error(out, "MSDF atlas generation failed");
                throw 1;
            }
            channels = 3;
            atlas_data.swap(bytes.data);
        } else {
            AtlasBytes<4> bytes;
            if (!generate_atlas_bytes<4, msdf_atlas::mtsdfGenerator>(glyphs, atlas_w, atlas_h, attributes, thread_count, bytes)) {
                set_error(out, "MTSDF atlas generation failed");
                throw 1;
            }
            channels = 4;
            atlas_data.swap(bytes.data);
        }

        std::string json = build_json(
            font_geometry,
            cfg->atlas_type,
            atlas_w,
            atlas_h,
            em_size,
            px_range,
            cfg->y_origin_top != 0
        );

        std::vector<uint8_t> artery_bytes;
        if (cfg->export_artery != 0) {
            if (!build_artery_font_binary(font_geometry, atlas_data, atlas_w, atlas_h, int(channels), cfg->atlas_type, em_size, px_range, artery_bytes)) {
                set_error(out, "Artery Font export failed");
                throw 1;
            }
        }

        const uint32_t atlas_len = static_cast<uint32_t>(atlas_data.size());
        uint8_t *atlas_ptr = static_cast<uint8_t *>(malloc(atlas_len));
        if (!atlas_ptr) {
            set_error(out, "Allocation failed for atlas output");
            throw 1;
        }
        memcpy(atlas_ptr, atlas_data.data(), atlas_len);

        ArenaString json_mem = alloc_copy(json);
        if (!json_mem.ptr && !json.empty()) {
            free(atlas_ptr);
            set_error(out, "Allocation failed for JSON output");
            throw 1;
        }

        uint8_t *artery_ptr = nullptr;
        uint32_t artery_len = static_cast<uint32_t>(artery_bytes.size());
        if (artery_len > 0) {
            artery_ptr = static_cast<uint8_t *>(malloc(artery_len));
            if (!artery_ptr) {
                free(atlas_ptr);
                free(json_mem.ptr);
                set_error(out, "Allocation failed for Artery output");
                throw 1;
            }
            memcpy(artery_ptr, artery_bytes.data(), artery_len);
        }

        out->ok = 1;
        out->atlas_width = static_cast<uint32_t>(atlas_w);
        out->atlas_height = static_cast<uint32_t>(atlas_h);
        out->atlas_channels = channels;
        out->atlas_ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(atlas_ptr));
        out->atlas_len = atlas_len;
        out->json_ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(json_mem.ptr));
        out->json_len = json_mem.len;
        out->artery_ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(artery_ptr));
        out->artery_len = artery_len;
        ok = 1;
    } catch (...) {
        if (!out->error_ptr) {
            set_error(out, "Unknown generation error");
        }
    }

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);
    return ok;
}

} // extern "C"
