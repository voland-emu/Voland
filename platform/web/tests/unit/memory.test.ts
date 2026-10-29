/**
 * Unit tests for src/memory.ts: the one shared memory's size (§4) agrees
 * with the two other places that state it. Growth is disabled, so a
 * mismatch would only surface as an instantiation failure in a browser.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { TOTAL_MEMORY_BYTES, TOTAL_MEMORY_PAGES, WASM_PAGE_BYTES } from "../../src/memory.ts";

const REPO_ROOT = new URL("../../../../", import.meta.url);

function repoFile(path: string): string {
  return readFileSync(new URL(path, REPO_ROOT), "utf8");
}

test("the memory is 65535 wasm32 pages: 4 GiB minus one page, never 2^32 (§4)", () => {
  assert.equal(WASM_PAGE_BYTES, 65_536);
  assert.equal(TOTAL_MEMORY_PAGES, 65_535);
  assert.equal(TOTAL_MEMORY_BYTES, 4_294_901_760);
  assert.ok(TOTAL_MEMORY_BYTES < 2 ** 32);
});

test("CMakeLists.txt's -sINITIAL_MEMORY equals TOTAL_MEMORY_BYTES (§24)", () => {
  const match = /-sINITIAL_MEMORY=(\d+)/.exec(repoFile("CMakeLists.txt"));
  assert.ok(match, "no -sINITIAL_MEMORY in CMakeLists.txt");
  assert.equal(Number(match[1]), TOTAL_MEMORY_BYTES);
});

test("CMakeLists.txt builds wasm32: no -m64 or MEMORY64 codegen flag (§24)", () => {
  const flagLines = repoFile("CMakeLists.txt")
    .split("\n")
    .map(line => line.replace(/#.*$/, "").trim())
    .filter(line => line.length > 0);
  assert.ok(!flagLines.some(line => /(^|\s)-m64(\s|$)|-sMEMORY64/.test(line)));
});

test("layout.h's LAYOUT_LINEAR_MEMORY_PAGES equals TOTAL_MEMORY_PAGES (§4)", () => {
  const match = /#define LAYOUT_LINEAR_MEMORY_PAGES \(\(uint64_t\)(\d+)\)/.exec(repoFile("core/common/layout.h"));
  assert.ok(match, "no LAYOUT_LINEAR_MEMORY_PAGES in core/common/layout.h");
  assert.equal(Number(match[1]), TOTAL_MEMORY_PAGES);
});
