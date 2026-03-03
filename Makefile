WASM_JS := web/msdf_atlas.js
WASM_WASM := web/msdf_atlas.wasm
EMXX ?= em++

MSDF_ATLAS_SOURCES := \
	msdf-atlas-gen/Charset.cpp \
	msdf-atlas-gen/GlyphGeometry.cpp \
	msdf-atlas-gen/FontGeometry.cpp \
	msdf-atlas-gen/Padding.cpp \
	msdf-atlas-gen/RectanglePacker.cpp \
	msdf-atlas-gen/TightAtlasPacker.cpp \
	msdf-atlas-gen/Workload.cpp \
	msdf-atlas-gen/bitmap-blit.cpp \
	msdf-atlas-gen/glyph-generators.cpp \
	msdf-atlas-gen/size-selectors.cpp \
	msdf-atlas-gen/utf8.cpp

MSDFGEN_SOURCES := \
	msdfgen/core/shape-description.cpp \
	msdfgen/core/save-rgba.cpp \
	msdfgen/core/save-tiff.cpp \
	msdfgen/core/save-bmp.cpp \
	msdfgen/core/sdf-error-estimation.cpp \
	msdfgen/core/save-fl32.cpp \
	msdfgen/core/render-sdf.cpp \
	msdfgen/core/msdfgen.cpp \
	msdfgen/core/rasterization.cpp \
	msdfgen/core/msdf-error-correction.cpp \
	msdfgen/core/export-svg.cpp \
	msdfgen/core/edge-selectors.cpp \
	msdfgen/core/edge-segments.cpp \
	msdfgen/core/equation-solver.cpp \
	msdfgen/core/edge-coloring.cpp \
	msdfgen/core/convergent-curve-ordering.cpp \
	msdfgen/core/contour-combiners.cpp \
	msdfgen/core/Shape.cpp \
	msdfgen/core/Scanline.cpp \
	msdfgen/core/Projection.cpp \
	msdfgen/core/MSDFErrorCorrection.cpp \
	msdfgen/core/EdgeHolder.cpp \
	msdfgen/core/Contour.cpp \
	msdfgen/core/DistanceMapping.cpp \
	msdfgen/ext/resolve-shape-geometry.cpp \
	msdfgen/ext/import-font.cpp

.PHONY: wasm serve clean

wasm: $(WASM_JS)

$(WASM_JS): wasm/atlas_wasm.cpp msdfgen/msdfgen-config.h $(MSDF_ATLAS_SOURCES) $(MSDFGEN_SOURCES)
	mkdir -p web
	$(EMXX) -O3 -std=c++17 -sWASM=1 -sMODULARIZE=1 -sEXPORT_NAME=MSDFAtlasWasm -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web -sFILESYSTEM=0 -sUSE_FREETYPE=1 -DMSDFGEN_DISABLE_SVG -DMSDFGEN_DISABLE_PNG -DMSDF_ATLAS_NO_ARTERY_FONT -I. -Iartery-font-format -Imsdf-atlas-gen -Imsdfgen -Imsdfgen/core -Imsdfgen/ext wasm/atlas_wasm.cpp $(MSDF_ATLAS_SOURCES) $(MSDFGEN_SOURCES) -o $(WASM_JS) -sEXPORTED_FUNCTIONS='["_mag_alloc","_mag_free","_mag_generate","_mag_release_result"]' -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8","HEAPU32","HEAPF32"]'

serve: wasm
	python3 -m http.server 8080 --directory web

clean:
	rm -f $(WASM_JS) $(WASM_WASM)
