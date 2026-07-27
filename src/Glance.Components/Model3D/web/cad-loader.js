const workerSource = "__GLANCE_OCCT_WORKER_SOURCE__";
const conversionTimeout = 120000;
let activeWorker = null;

document.getElementById("progress-track")?.remove();

function cadFormat(extension) {
    switch (extension) {
    case ".step":
    case ".stp":
        return "step";
    case ".iges":
    case ".igs":
        return "iges";
    case ".brep":
    case ".brp":
        return "brep";
    default:
        return null;
    }
}

function materialForColor(color, darkTheme, THREE) {
    const fallback = darkTheme ? 0x9cb8d0 : 0x557a95;
    const value = Array.isArray(color) && color.length >= 3
        ? new THREE.Color(color[0], color[1], color[2])
        : fallback;
    return new THREE.MeshStandardMaterial({
        color: value,
        metalness: 0.08,
        roughness: 0.72,
    });
}

function buildCadModel(payload, darkTheme, THREE) {
    const geometries = [];
    const materials = [];
    for (const source of payload.meshes) {
        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute(
            "position",
            new THREE.BufferAttribute(source.position, 3));
        if (source.normal) {
            geometry.setAttribute(
                "normal",
                new THREE.BufferAttribute(source.normal, 3));
        } else {
            geometry.computeVertexNormals();
        }
        geometry.setIndex(new THREE.BufferAttribute(source.index, 1));

        const meshMaterials = [
            materialForColor(source.color, darkTheme, THREE),
        ];
        let groupCount = 0;
        if (source.faces.length > 0) {
            const colorMaterials = new Map();
            const triangleCount = source.index.length / 3;
            geometry.clearGroups();
            for (const face of source.faces) {
                const first = Number(face.first);
                const last = Math.min(Number(face.last), triangleCount - 1);
                if (!Number.isInteger(first) ||
                    !Number.isInteger(last) ||
                    first < 0 ||
                    first >= triangleCount ||
                    last < first) {
                    continue;
                }
                let materialIndex = 0;
                if (Array.isArray(face.color) && face.color.length >= 3) {
                    const key = face.color.slice(0, 3).join(",");
                    if (!colorMaterials.has(key)) {
                        colorMaterials.set(key, meshMaterials.length);
                        meshMaterials.push(
                            materialForColor(face.color, darkTheme, THREE));
                    }
                    materialIndex = colorMaterials.get(key);
                }
                geometry.addGroup(
                    first * 3,
                    (last - first + 1) * 3,
                    materialIndex);
                ++groupCount;
            }
        }
        geometries.push(geometry);
        materials.push(groupCount > 0 ? meshMaterials : meshMaterials[0]);
    }

    const referenced = new Set();
    const buildNode = (node) => {
        const group = new THREE.Group();
        group.name = node?.name || "";
        for (const meshIndex of node?.meshes || []) {
            if (!Number.isInteger(meshIndex) ||
                meshIndex < 0 ||
                meshIndex >= geometries.length) {
                continue;
            }
            referenced.add(meshIndex);
            const mesh = new THREE.Mesh(
                geometries[meshIndex],
                materials[meshIndex]);
            mesh.name = payload.meshes[meshIndex].name || "";
            group.add(mesh);
        }
        for (const child of node?.children || []) {
            group.add(buildNode(child));
        }
        return group;
    };

    const root = buildNode(payload.root);
    for (let index = 0; index < geometries.length; ++index) {
        if (referenced.has(index)) {
            continue;
        }
        const mesh = new THREE.Mesh(geometries[index], materials[index]);
        mesh.name = payload.meshes[index].name || "";
        root.add(mesh);
    }
    return root;
}

function stopWorker() {
    activeWorker?.terminate();
    activeWorker = null;
}

window.addEventListener("pagehide", stopWorker);

window.loadCadModel = (modelUrl, extension, darkTheme, THREE) => {
    const format = cadFormat(extension);
    if (!format) {
        return Promise.reject(new Error("Unsupported CAD format"));
    }

    stopWorker();
    const workerUrl = URL.createObjectURL(new Blob(
        [workerSource],
        { type: "text/javascript" }));
    const worker = new Worker(workerUrl);
    URL.revokeObjectURL(workerUrl);
    activeWorker = worker;

    return new Promise((resolve, reject) => {
        let completed = false;
        const finish = (callback, value) => {
            if (completed) {
                return;
            }
            completed = true;
            clearTimeout(timeout);
            if (activeWorker === worker) {
                activeWorker = null;
            }
            worker.terminate();
            callback(value);
        };
        const timeout = setTimeout(
            () => finish(reject, new Error("CAD conversion timed out")),
            conversionTimeout);

        worker.onmessage = (event) => {
            if (event.data?.type === "result") {
                try {
                    finish(
                        resolve,
                        buildCadModel(event.data, darkTheme, THREE));
                } catch (error) {
                    finish(reject, error);
                }
                return;
            }
            finish(
                reject,
                new Error(event.data?.message || "CAD conversion failed"));
        };
        worker.onerror = (event) => {
            finish(reject, new Error(event.message || "CAD worker failed"));
        };
        worker.onmessageerror = () => {
            finish(reject, new Error("CAD worker returned invalid data"));
        };
        worker.postMessage({
            modelUrl,
            wasmUrl: new URL(
                "./vendor/occt/occt-import-js.wasm",
                window.location.href).href,
            format,
        });
    });
};
