import * as THREE from "../third_party/three/bin/r184/package/build/three.module.min.js";
import { OrbitControls } from "../third_party/three/bin/r184/package/examples/jsm/controls/OrbitControls.js";
import { STLLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/STLLoader.js";
import { ThreeMFLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/3MFLoader.js";
import { OBJLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/OBJLoader.js";
import { MTLLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/MTLLoader.js";
import { PLYLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/PLYLoader.js";
import { GLTFLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/GLTFLoader.js";
import { DRACOLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/DRACOLoader.js";
import { FBXLoader } from "../third_party/three/bin/r184/package/examples/jsm/loaders/FBXLoader.js";
import { MeshoptDecoder } from "../third_party/three/bin/r184/package/examples/jsm/libs/meshopt_decoder.module.js";

const parameters = new URLSearchParams(window.location.search);
const modelUrl = parameters.get("model");
const extension = parameters.get("extension")?.toLowerCase();
const labels = {
    loading: parameters.get("loading") || "Viewer.Loading",
    failed: parameters.get("failed") || "Viewer.Failed",
    empty: parameters.get("empty") || "Viewer.Empty",
    fit: parameters.get("fit") || "Viewer.Fit",
    grid: parameters.get("grid") || "Viewer.Grid",
    wireframe: parameters.get("wireframe") || "Viewer.Wireframe",
};

document.documentElement.dataset.theme =
    parameters.get("theme") === "dark" ? "dark" : "light";

const canvas = document.getElementById("canvas");
const status = document.getElementById("status");
const statusText = document.getElementById("status-text");
const progressValue = document.getElementById("progress-value");
const fitButton = document.getElementById("fit");
const gridButton = document.getElementById("grid");
const wireframeButton = document.getElementById("wireframe");

fitButton.title = labels.fit;
fitButton.setAttribute("aria-label", labels.fit);
gridButton.title = labels.grid;
gridButton.setAttribute("aria-label", labels.grid);
wireframeButton.title = labels.wireframe;
wireframeButton.setAttribute("aria-label", labels.wireframe);
statusText.textContent = labels.loading;

const darkTheme = document.documentElement.dataset.theme === "dark";
const scene = new THREE.Scene();
scene.background = new THREE.Color(darkTheme ? 0x202020 : 0xf5f5f5);

const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000);
camera.position.set(3, 2, 3);

let renderer;
try {
    renderer = new THREE.WebGLRenderer({
        canvas,
        antialias: true,
        alpha: false,
        powerPreference: "high-performance",
    });
} catch (error) {
    status.classList.add("error");
    statusText.textContent = labels.failed;
    window.reportViewerError(error?.message || String(error));
    throw error;
}
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1;
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));

const controls = new OrbitControls(camera, canvas);
controls.enableDamping = false;
controls.screenSpacePanning = true;

scene.add(new THREE.HemisphereLight(0xffffff, 0x606060, darkTheme ? 2.2 : 1.8));
const keyLight = new THREE.DirectionalLight(0xffffff, 2.4);
keyLight.position.set(3, 5, 4);
scene.add(keyLight);
const fillLight = new THREE.DirectionalLight(0xffffff, 1.2);
fillLight.position.set(-4, 2, -3);
scene.add(fillLight);

let model = null;
let modelBounds = null;
let grid = null;
let wireframe = false;
let renderRequested = false;

function requestRender() {
    if (renderRequested) {
        return;
    }
    renderRequested = true;
    requestAnimationFrame(() => {
        renderRequested = false;
        renderer.render(scene, camera);
    });
}

function resize() {
    const width = Math.max(1, canvas.clientWidth);
    const height = Math.max(1, canvas.clientHeight);
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    requestRender();
}

function progress(event) {
    if (event.lengthComputable && event.total > 0) {
        const percentage = Math.min(100, Math.round(event.loaded * 100 / event.total));
        progressValue.style.width = `${percentage}%`;
    }
}

function loadWith(loader, url) {
    return new Promise((resolve, reject) => {
        loader.load(url, resolve, progress, reject);
    });
}

function geometryMaterial(geometry) {
    return new THREE.MeshStandardMaterial({
        color: darkTheme ? 0x9cb8d0 : 0x557a95,
        metalness: 0.08,
        roughness: 0.72,
        vertexColors: geometry.hasAttribute("color"),
    });
}

async function loadObj(url) {
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
    }
    const source = await response.text();
    const loader = new OBJLoader();
    const materialReference = source.match(/^[ \t]*mtllib[ \t]+(.+?)[ \t]*$/im);
    if (materialReference) {
        try {
            const materialUrl = new URL(materialReference[1], url).href;
            const materials = await loadWith(new MTLLoader(), materialUrl);
            materials.preload();
            loader.setMaterials(materials);
        } catch {
        }
    }
    return loader.parse(source);
}

async function loadModel() {
    switch (extension) {
    case ".stl": {
        const geometry = await loadWith(new STLLoader(), modelUrl);
        if (!geometry.hasAttribute("normal")) {
            geometry.computeVertexNormals();
        }
        return new THREE.Mesh(geometry, geometryMaterial(geometry));
    }
    case ".3mf":
        return loadWith(new ThreeMFLoader(), modelUrl);
    case ".obj":
        return loadObj(modelUrl);
    case ".ply": {
        const geometry = await loadWith(new PLYLoader(), modelUrl);
        if (!geometry.hasAttribute("normal")) {
            geometry.computeVertexNormals();
        }
        return new THREE.Mesh(geometry, geometryMaterial(geometry));
    }
    case ".gltf":
    case ".glb": {
        const draco = new DRACOLoader();
        draco.setDecoderPath("./vendor/draco/");
        draco.setDecoderConfig({ type: "wasm" });
        const loader = new GLTFLoader();
        loader.setDRACOLoader(draco);
        loader.setMeshoptDecoder(MeshoptDecoder);
        try {
            const result = await loadWith(loader, modelUrl);
            return result.scene;
        } finally {
            draco.dispose();
        }
    }
    case ".fbx":
        return loadWith(new FBXLoader(), modelUrl);
    default:
        throw new Error("Unsupported model format");
    }
}

function fitModel() {
    if (!modelBounds || modelBounds.isEmpty()) {
        return;
    }
    const size = modelBounds.getSize(new THREE.Vector3());
    const center = modelBounds.getCenter(new THREE.Vector3());
    const maximumSize = Math.max(size.x, size.y, size.z, 0.0001);
    const fitHeightDistance =
        maximumSize / (2 * Math.tan(THREE.MathUtils.degToRad(camera.fov * 0.5)));
    const fitWidthDistance = fitHeightDistance / Math.max(camera.aspect, 0.01);
    const distance = Math.max(fitHeightDistance, fitWidthDistance) * 1.35;
    const direction = camera.position.clone()
        .sub(controls.target)
        .normalize();
    if (direction.lengthSq() < 0.01) {
        direction.set(1, 0.7, 1).normalize();
    }
    controls.target.copy(center);
    camera.position.copy(center).addScaledVector(direction, distance);
    camera.near = Math.max(distance / 1000, maximumSize / 100000, 0.0001);
    camera.far = Math.max(distance * 100, maximumSize * 100);
    camera.updateProjectionMatrix();
    controls.minDistance = Math.max(maximumSize / 1000, 0.0001);
    controls.maxDistance = Math.max(maximumSize * 100, distance * 10);
    controls.update();
    requestRender();
}

function updateGrid() {
    if (grid) {
        scene.remove(grid);
        grid.geometry.dispose();
        grid.material.dispose();
        grid = null;
    }
    if (gridButton.getAttribute("aria-pressed") !== "true" || !modelBounds) {
        requestRender();
        return;
    }
    const size = modelBounds.getSize(new THREE.Vector3());
    const center = modelBounds.getCenter(new THREE.Vector3());
    const extent = Math.max(size.x, size.y, size.z);
    grid = new THREE.GridHelper(
        Math.max(extent * 2, 1),
        20,
        darkTheme ? 0x6f6f6f : 0x777777,
        darkTheme ? 0x3c3c3c : 0xc7c7c7);
    grid.position.set(center.x, modelBounds.min.y, center.z);
    scene.add(grid);
    requestRender();
}

function updateWireframe() {
    if (!model) {
        return;
    }
    model.traverse((object) => {
        if (!object.isMesh) {
            return;
        }
        const materials = Array.isArray(object.material)
            ? object.material
            : [object.material];
        for (const material of materials) {
            if (material && "wireframe" in material) {
                material.wireframe = wireframe;
                material.needsUpdate = true;
            }
        }
    });
    requestRender();
}

function disposeObject(root) {
    root?.traverse((object) => {
        object.geometry?.dispose();
        const materials = Array.isArray(object.material)
            ? object.material
            : [object.material];
        for (const material of materials) {
            if (!material) {
                continue;
            }
            for (const value of Object.values(material)) {
                if (value?.isTexture) {
                    value.dispose();
                }
            }
            material.dispose();
        }
    });
}

controls.addEventListener("change", requestRender);
fitButton.addEventListener("click", fitModel);
canvas.addEventListener("dblclick", fitModel);
gridButton.addEventListener("click", () => {
    const enabled = gridButton.getAttribute("aria-pressed") !== "true";
    gridButton.setAttribute("aria-pressed", String(enabled));
    updateGrid();
});
wireframeButton.addEventListener("click", () => {
    wireframe = !wireframe;
    wireframeButton.setAttribute("aria-pressed", String(wireframe));
    updateWireframe();
});

new ResizeObserver(resize).observe(document.getElementById("viewport"));
resize();

window.addEventListener("pagehide", () => {
    disposeObject(model);
    if (grid) {
        grid.geometry.dispose();
        grid.material.dispose();
    }
    controls.dispose();
    renderer.dispose();
});

async function showModel() {
    try {
        if (!modelUrl || !extension) {
            throw new Error("Missing model URL");
        }
        model = await loadModel();
        modelBounds = new THREE.Box3().setFromObject(model);
        if (modelBounds.isEmpty() ||
            !Number.isFinite(modelBounds.min.x) ||
            !Number.isFinite(modelBounds.max.x)) {
            throw new Error(labels.empty);
        }
        scene.add(model);
        updateGrid();
        fitModel();
        status.hidden = true;
        requestRender();
        window.chrome?.webview?.postMessage("model3d: loaded");
    } catch (error) {
        status.classList.add("error");
        statusText.textContent =
            error?.message === labels.empty
                ? labels.empty
                : labels.failed;
        window.reportViewerError(error?.message || String(error));
    }
}

showModel();
