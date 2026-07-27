function readCadFile(occt, format, source) {
    switch (format) {
    case "step":
        return occt.ReadStepFile(source, null);
    case "iges":
        return occt.ReadIgesFile(source, null);
    case "brep":
        return occt.ReadBrepFile(source, null);
    default:
        throw new Error("Unsupported CAD format");
    }
}

self.onmessage = async (event) => {
    try {
        const { modelUrl, wasmUrl, format } = event.data;
        const response = await fetch(modelUrl);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const source = new Uint8Array(await response.arrayBuffer());
        const occt = await occtimportjs({
            locateFile: (path) => path.endsWith(".wasm") ? wasmUrl : path,
        });
        const result = readCadFile(occt, format, source);
        if (!result?.success) {
            throw new Error("CAD conversion failed");
        }

        const transfers = [];
        const meshes = result.meshes.map((mesh) => {
            const position = Float32Array.from(mesh.attributes.position.array);
            const normal = mesh.attributes.normal
                ? Float32Array.from(mesh.attributes.normal.array)
                : null;
            const index = Uint32Array.from(mesh.index.array);
            transfers.push(position.buffer, index.buffer);
            if (normal) {
                transfers.push(normal.buffer);
            }
            return {
                name: mesh.name || "",
                color: mesh.color || null,
                faces: (mesh.brep_faces || []).map((face) => ({
                    first: face.first,
                    last: face.last,
                    color: face.color || null,
                })),
                position,
                normal,
                index,
            };
        });

        self.postMessage({
            type: "result",
            root: result.root,
            meshes,
        }, transfers);
    } catch (error) {
        self.postMessage({
            type: "error",
            message: error?.message || String(error),
        });
    } finally {
        self.close();
    }
};
