import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";
import * as THREE from "../../src/Glance.Components/Model3D/third_party/three/bin/r184/package/build/three.module.min.js";

const context = vm.createContext({
    document: { getElementById: () => null },
    window: { addEventListener() {} },
});
vm.runInContext(await readFile(new URL(
    "../../src/Glance.Components/Model3D/web/cad-loader.js", import.meta.url), "utf8"), context);

function build(faces, triangleCount, color = [0.2, 0.4, 0.6]) {
    const index = Uint32Array.from({ length: triangleCount * 3 }, (_, i) => i);
    const payload = {
        meshes: [{
            position: new Float32Array(triangleCount * 9),
            normal: new Float32Array(triangleCount * 9),
            index,
            faces,
            color,
        }],
    };
    const root = context.buildCadModel(payload, false, THREE);
    assert.equal(root.children.length, 1);
    const mesh = root.children[0];
    assert.deepEqual(mesh.geometry.index.array, index, "Triangle order is preserved");
    return mesh;
}

const faceCount = 10000;
const uniform = build(Array.from({ length: faceCount }, (_, i) => ({
    first: i, last: i, color: i % 2 ? [0.2, 0.4, 0.6] : undefined,
})), faceCount);
assert.deepEqual(uniform.geometry.groups, [{
    start: 0, count: faceCount * 3, materialIndex: 0,
}]);
assert.equal(uniform.material.length, 1, "Explicit base colors reuse the base material");

const mixed = build([
    { first: 0, last: 1 },
    { first: 2, last: 2, color: [1, 0, 0] },
    { first: 3, last: 4, color: [1, 0, 0] },
    { first: 5, last: 5 },
], 6);
assert.deepEqual(mixed.geometry.groups, [
    { start: 0, count: 6, materialIndex: 0 },
    { start: 6, count: 9, materialIndex: 1 },
    { start: 15, count: 3, materialIndex: 0 },
]);
assert.equal(mixed.material.length, 2);
assert.equal(mixed.material[1].color.getHex(), new THREE.Color(1, 0, 0).getHex());

const separated = build([
    { first: 0, last: 0 },
    { first: -1, last: 1 },
    { first: 2, last: 2 },
    { first: 2, last: 3 },
], 4);
assert.deepEqual(separated.geometry.groups, [
    { start: 0, count: 3, materialIndex: 0 },
    { start: 6, count: 3, materialIndex: 0 },
    { start: 6, count: 6, materialIndex: 0 },
], "Gaps and overlapping ranges retain their original semantics");
assert.equal(Array.isArray(build([], 1).material), false);
console.log(`CAD regression passed: ${faceCount} adjacent faces -> 1 draw group; colors and indices preserved`);
