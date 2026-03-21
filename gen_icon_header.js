#!/usr/bin/env node
// gen_icon_header.js
// 用法: node gen_icon_header.js [input.ico] [output.h]
// 默认: node gen_icon_header.js pen_mouse.ico icon_data.h

import { readFileSync, writeFileSync } from "fs";
import { basename } from "path";

// ── 参数解析 ──────────────────────────────────────────────
const [, , icoPath = "pen_mouse.ico", outPath = "icon_data.h"] = process.argv;

const ico = readFileSync(icoPath);

// ── 验证 ICO 文件头 ────────────────────────────────────────
const reserved = ico.readUInt16LE(0);
const type     = ico.readUInt16LE(2);
const count    = ico.readUInt16LE(4);

if (reserved !== 0 || type !== 1) {
  console.error(`错误：${icoPath} 不是有效的 ICO 文件`);
  process.exit(1);
}

// ── 解析每个尺寸条目（用于注释）─────────────────────────
const sizes = [];
for (let i = 0; i < count; i++) {
  const base = 6 + i * 16;
  const w = ico[base] === 0 ? 256 : ico[base];
  const h = ico[base + 1] === 0 ? 256 : ico[base + 1];
  const sz = ico.readUInt32LE(base + 8);
  sizes.push(`${w}×${h} (${(sz / 1024).toFixed(1)} KB)`);
}

// ── 把字节数组格式化为 C++ 十六进制字面量 ───────────────
const COLS = 16;
const hexLines = [];
for (let i = 0; i < ico.length; i += COLS) {
  const chunk = ico.slice(i, i + COLS);
  const hex = Array.from(chunk)
    .map((b) => `0x${b.toString(16).padStart(2, "0")}`)
    .join(", ");
  const comma = i + COLS < ico.length ? "," : "";
  hexLines.push(`    ${hex}${comma}`);
}

// ── 生成头文件内容 ────────────────────────────────────────
const srcName  = basename(icoPath);
const varName  = "kIconData";
const sizeName = "kIconSize";

const header = `\
// ${basename(outPath)}
// 由 gen_icon_header.js 自动生成，请勿手动编辑
// 源文件: ${srcName}  (${ico.length} bytes)
// 内含尺寸: ${sizes.join(", ")}
//
// 使用方式（在 pen_mouse.cpp 中）:
//   #include "${basename(outPath)}"
//   wc.hIcon   = LoadEmbeddedIcon(32, 32);
//   wc.hIconSm = LoadEmbeddedIcon(16, 16);

#pragma once
#include <windows.h>

// ── 原始 ICO 字节数据 ──────────────────────────────────────
static const unsigned char ${varName}[] = {
${hexLines.join("\n")}
};
static const DWORD ${sizeName} = ${ico.length}UL;

// ── 从嵌入数据加载指定尺寸的 HICON ────────────────────────
// cx / cy: 所需像素尺寸（如 32、16）
// 自动选取 ICO 中最接近且不超过请求尺寸的帧；
// 若所有帧都大于请求尺寸则退化为最小帧。
static HICON LoadEmbeddedIcon(int cx, int cy) {
    const BYTE* p = ${varName};

    // ICO 目录条目结构（固定 16 字节）
    struct IcoEntry {
        BYTE  width;       // 0 表示 256
        BYTE  height;
        BYTE  colorCount;
        BYTE  reserved;
        WORD  planes;
        WORD  bitCount;
        DWORD dataSize;
        DWORD dataOffset;
    };

    WORD nImages = *reinterpret_cast<const WORD*>(p + 4);
    const IcoEntry* dir = reinterpret_cast<const IcoEntry*>(p + 6);

    // 找最接近且不超过 cx 的帧
    int bestIdx = 0;
    int bestW   = 0;
    for (int i = 0; i < nImages; ++i) {
        int w = (dir[i].width == 0) ? 256 : dir[i].width;
        if (w <= cx && w > bestW) {
            bestW   = w;
            bestIdx = i;
        }
    }
    // 全部帧都大于 cx：退化为最小帧
    if (bestW == 0) {
        int minW = INT_MAX;
        for (int i = 0; i < nImages; ++i) {
            int w = (dir[i].width == 0) ? 256 : dir[i].width;
            if (w < minW) { minW = w; bestIdx = i; }
        }
    }

    const BYTE* imgData = p + dir[bestIdx].dataOffset;
    DWORD       imgSize = dir[bestIdx].dataSize;

    // PNG 帧（Vista+）或旧式 BMP 帧均可由 CreateIconFromResourceEx 处理
    HICON hIcon = CreateIconFromResourceEx(
        const_cast<PBYTE>(imgData), imgSize,
        TRUE,           // fIcon = TRUE
        0x00030000,     // dwVer = 3.0
        cx, cy,
        LR_DEFAULTCOLOR
    );
    return hIcon;
}
`;

// ── 写出文件 ──────────────────────────────────────────────
writeFileSync(outPath, header, "utf8");

console.log(`✓ 生成完成`);
console.log(`  输入: ${icoPath}  (${ico.length} bytes, ${count} 帧)`);
console.log(`  尺寸: ${sizes.join("  ")}`);
console.log(`  输出: ${outPath}  (${header.length} chars)`);
