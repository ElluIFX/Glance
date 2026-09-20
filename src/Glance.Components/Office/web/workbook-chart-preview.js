import { buildPresentation, renderSlide } from './presentation-engine.mjs';

function escapeXml(value) {
    return value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}

export async function renderWorkbookChart(parts, theme, container, width, height, signal) {
    signal?.throwIfAborted();
    if (![width, height].every(value => Number.isFinite(value) && value > 0 && value <= 32768)) throw new Error('Invalid chart size');
    const p = 'http://schemas.openxmlformats.org/presentationml/2006/main';
    const a = 'http://schemas.openxmlformats.org/drawingml/2006/main';
    const r = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
    const relationNamespace = 'http://schemas.openxmlformats.org/package/2006/relationships';
    const cx = Math.round(width * 9525), cy = Math.round(height * 9525);
    // Feed the existing OOXML chart renderer through its public presentation model API.
    const files = {
        contentTypes: '',
        presentation: `<p:presentation xmlns:p="${p}" xmlns:r="${r}"><p:sldIdLst><p:sldId id="256" r:id="slide"/></p:sldIdLst><p:sldSz cx="${cx}" cy="${cy}"/></p:presentation>`,
        presentationRels: `<Relationships xmlns="${relationNamespace}"><Relationship Id="slide" Type="${r}/slide" Target="slides/slide1.xml"/></Relationships>`,
    };
    for (const name of ['slides', 'slideRels', 'slideLayouts', 'slideLayoutRels', 'slideMasters', 'slideMasterRels', 'themes', 'media', 'charts', 'chartRels', 'chartStyles', 'chartColors', 'themeOverrides', 'diagramDrawings']) files[name] = new Map();
    files.slides.set('ppt/slides/slide1.xml', `<p:sld xmlns:p="${p}" xmlns:a="${a}" xmlns:r="${r}"><p:cSld><p:spTree><p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="2" name="Chart"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x="0" y="0"/><a:ext cx="${cx}" cy="${cy}"/></p:xfrm><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="chart"/></a:graphicData></a:graphic></p:graphicFrame></p:spTree></p:cSld></p:sld>`);
    files.slideRels.set('ppt/slides/_rels/slide1.xml.rels', `<Relationships xmlns="${relationNamespace}"><Relationship Id="chart" Type="${r}/chart" Target="../../${escapeXml(parts.path)}"/></Relationships>`);
    files.charts.set(parts.path, parts.xml);
    if (parts.rels) files.chartRels.set(parts.relPath, parts.rels);
    for (const [path, xml] of parts.styles) files.chartStyles.set(path, xml);
    for (const [path, xml] of parts.colors) files.chartColors.set(path, xml);
    for (const [path, xml] of parts.themes) files.themeOverrides.set(path, xml);
    if (theme) files.themes.set('xl/theme/theme1.xml', theme);
    const presentation = buildPresentation(files);
    if (theme && !presentation.chartThemes.has(parts.path)) presentation.chartThemes.set(parts.path, presentation.themes.get('xl/theme/theme1.xml'));
    const charts = new Set();
    let failure;
    const handle = renderSlide(presentation, presentation.slides[0], {
        chartInstances: charts,
        onNodeError: (_id, error) => { failure = error; },
    });
    const dispose = () => { signal?.removeEventListener('abort', dispose); handle.dispose(); container.replaceChildren(); };
    signal?.addEventListener('abort', dispose, { once: true });
    try {
        container.replaceChildren(handle.element);
        await handle.ready;
        signal?.throwIfAborted();
        if (failure) throw failure;
        if (charts.size !== 1) throw new Error('Chart rendering failed');
        for (const chart of charts) {
            const option = chart.getOption();
            option.animation = false;
            for (const series of option.series ?? []) series.animation = false;
            chart.setOption(option, { notMerge: true, lazyUpdate: false });
        }
        return { dispose };
    } catch (error) { dispose(); throw error; }
}
