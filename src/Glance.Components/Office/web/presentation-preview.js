import { PptxViewer, RECOMMENDED_ZIP_LIMITS } from './presentation-engine.mjs';

export async function renderPresentationPreview(bytes, container, { signal, onFirstReady, onError } = {}) {
    signal?.throwIfAborted();
    if (bytes.byteLength > 128 * 1024 * 1024) throw new Error('Presentation exceeds preview budget');
    let failure;
    const reportError = (_id, error) => {
        failure = error instanceof Error ? error : new Error(String(error));
        if (!signal?.aborted) onError?.(failure);
    };
    const viewer = new PptxViewer(container, {
        zipLimits: RECOMMENDED_ZIP_LIMITS,
        lazySlides: true,
        lazyMedia: true,
        pdfjs: false,
        onSlideError: reportError,
        onNodeError: reportError,
    });
    let disposed = false;
    const dispose = () => {
        if (disposed) return;
        disposed = true;
        signal?.removeEventListener('abort', dispose);
        viewer.destroy();
    };
    signal?.addEventListener('abort', dispose, { once: true });
    try {
        await viewer.open(bytes, {
            signal,
            lazySlides: true,
            lazyMedia: true,
            // Windowed batches create placeholders; slide content remains viewport-driven.
            listOptions: { windowed: true, initialSlides: 1, batchSize: 64 },
        });
        signal?.throwIfAborted();
        if (viewer.slideCount === 0) throw new Error('Presentation has no slides');
        await document.fonts.ready;
        await Promise.all([...container.querySelectorAll('img')].filter(image => {
            const bounds = image.getBoundingClientRect();
            return bounds.bottom > 0 && bounds.top < innerHeight;
        }).map(image => image.decode()));
        signal?.throwIfAborted();
        if (failure) throw failure;
        onFirstReady?.();
        return { viewer, dispose };
    } catch (error) {
        dispose();
        throw error;
    }
}
