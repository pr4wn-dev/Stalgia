#pragma once

#include "core/ComPtr.h"

#include <d3d11.h>
#include <string>
#include <vector>

class GlyphAtlas {
public:
    // glyphSet: 0 katakana, 1 latin, 2 binary, 3 custom (falls back to latin if empty).
    bool build(ID3D11Device* device, int cellSize, int glyphSet = 0, const std::wstring& custom = {});
    void release();

    int glyphCount() const { return (int)m_charset.size(); }
    int cellSize() const { return m_cell; }
    int columns() const { return m_cols; }
    int rows() const { return m_rows; }

    void uvFor(int glyphIndex, float& u0, float& v0, float& u1, float& v1) const;

    ID3D11ShaderResourceView* srv() const { return m_srv.get(); }

private:
    std::wstring m_charset;
    int m_flipFrom = 0;  // glyphs >= this index are drawn mirrored
    int m_cell = 0;
    int m_cols = 0;
    int m_rows = 0;
    int m_texW = 0;
    int m_texH = 0;
    ComPtr<ID3D11Texture2D> m_tex;
    ComPtr<ID3D11ShaderResourceView> m_srv;

    static std::wstring buildCharset();
    static HFONT tryCreateFont(int height);
};
