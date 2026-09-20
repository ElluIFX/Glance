import SSF from 'ssf';

function axis(count, fallback, settings, convert) {
    const offsets = new Float64Array(count + 1);
    offsets.fill(convert(fallback), 1);
    for (const item of settings) {
        if (item.index > 0 && item.index <= count) offsets[item.index] = item.hidden ? 0 : convert(item.height ?? item.width ?? fallback);
    }
    for (let index = 1; index <= count; ++index) offsets[index] += offsets[index - 1];
    return offsets;
}

function upperBound(values, value) {
    let low = 0, high = values.length;
    while (low < high) {
        const middle = (low + high) >>> 1;
        if (values[middle] <= value) low = middle + 1;
        else high = middle;
    }
    return low;
}

function visibleIndices(offsets, start, length, limit) {
    const result = [];
    let index = Math.max(0, upperBound(offsets, Math.max(0, start - 40)) - 1);
    while (index + 1 < offsets.length && offsets[index] < start + length + 40 && result.length < limit) {
        result.push(index + 1);
        index = upperBound(offsets, offsets[index + 1]) - 1;
    }
    return result;
}

function columnName(index) {
    let name = '';
    while (index > 0) { --index; name = String.fromCharCode(65 + index % 26) + name; index = Math.floor(index / 26); }
    return name;
}

function color(value, theme = []) {
    const rgb = value?.argb?.slice(-6) ?? theme[value?.theme];
    if (!/^[0-9a-f]{6}$/i.test(rgb ?? '')) return undefined;
    if (!Number.isFinite(value?.tint) || value.tint === 0) return `#${rgb}`;
    const [red, green, blue] = [0, 2, 4].map(offset => parseInt(rgb.slice(offset, offset + 2), 16) / 255);
    const maximum = Math.max(red, green, blue), minimum = Math.min(red, green, blue);
    const delta = maximum - minimum, lightness = (maximum + minimum) / 2;
    const saturation = delta === 0 ? 0 : delta / (1 - Math.abs(2 * lightness - 1));
    let hue = delta === 0 ? 0 : maximum === red ? (green - blue) / delta : maximum === green ? (blue - red) / delta + 2 : (red - green) / delta + 4;
    hue = (hue + 6) % 6;
    const tint = Math.max(-1, Math.min(1, value.tint));
    const adjusted = tint < 0 ? lightness * (1 + tint) : lightness * (1 - tint) + tint;
    return `hsl(${hue * 60} ${saturation * 100}% ${adjusted * 100}%)`;
}

function fontStyle(element, font = {}, theme) {
    if (font.name) element.style.fontFamily = font.name;
    if (font.size) element.style.fontSize = `${font.size}pt`;
    if (font.bold) element.style.fontWeight = 'bold';
    if (font.italic) element.style.fontStyle = 'italic';
    const decorations = [font.underline ? 'underline' : '', font.strike ? 'line-through' : ''].filter(Boolean);
    if (decorations.length) element.style.textDecoration = decorations.join(' ');
    const foreground = color(font.color, theme);
    if (foreground) element.style.color = foreground;
}

function populateCell(element, cell, { date1904, theme }) {
    const style = cell?.style ?? {};
    let value = cell?.result ?? cell?.value ?? cell?.text ?? '';
    if (value instanceof Date) value = value.getTime() / 86400000 + 25569 - (date1904 ? 1462 : 0);
    if (typeof value === 'number') {
        try { element.textContent = SSF.format(style.numFmt || 'General', value, { date1904 }); }
        catch { element.textContent = String(value); }
        element.style.textAlign = 'right';
    } else if (value?.richText) {
        for (const run of value.richText) {
            const span = document.createElement('span');
            span.textContent = run.text;
            fontStyle(span, run.font, theme);
            element.append(span);
        }
    } else element.textContent = value?.error ?? String(value);
    fontStyle(element, style.font, theme);
    const background = color(style.fill?.fgColor, theme);
    if (background && style.fill?.pattern === 'solid') element.style.backgroundColor = background;
    const alignment = style.alignment ?? {};
    if (alignment.horizontal) element.style.textAlign = alignment.horizontal === 'centerContinuous' ? 'center' : alignment.horizontal;
    if (alignment.wrapText) element.style.whiteSpace = 'pre-wrap';
    if (alignment.indent) element.style.paddingLeft = `${alignment.indent * 8 + 3}px`;
    if (alignment.vertical) element.style.alignContent = alignment.vertical === 'middle' ? 'center' : alignment.vertical === 'bottom' ? 'end' : 'start';
    for (const side of ['top', 'right', 'bottom', 'left']) {
        const border = style.border?.[side];
        if (!border?.style) continue;
        const width = border.style === 'thick' ? 3 : border.style.startsWith('medium') ? 2 : 1;
        const line = border.style === 'double' ? 'double' : /dash/i.test(border.style) ? 'dashed' : /dot/i.test(border.style) ? 'dotted' : 'solid';
        element.style[`border${side[0].toUpperCase()}${side.slice(1)}`] = `${width}px ${line} ${color(border.color, theme) ?? '#000'}`;
    }
}

export async function renderWorkbookPreview(bytes, container, { signal, onFirstReady, onSourceConsumed, onError, convertImage, dataSource } = {}) {
    signal?.throwIfAborted();
    const worker = dataSource ?? new Worker(new URL('./workbook-worker.mjs', import.meta.url), { type: 'module' });
    container.classList.add('workbook');
    container.innerHTML = '<div class="workbook-body"><div class="workbook-corner"></div><div class="workbook-column-headers"></div><div class="workbook-row-headers"></div><div class="workbook-viewport" tabindex="0"><div class="workbook-stage"></div></div></div><div class="workbook-tabs" role="tablist"></div>';
    const viewport = container.querySelector('.workbook-viewport');
    const stage = container.querySelector('.workbook-stage');
    const cellLayer = document.createElement('div');
    const chartLayer = document.createElement('div');
    stage.append(cellLayer, chartLayer);
    const rowHeaders = container.querySelector('.workbook-row-headers');
    const columnHeaders = container.querySelector('.workbook-column-headers');
    const tabs = container.querySelector('.workbook-tabs');
    let metadata, rowOffsets, columnOffsets;
    let generation = 1, requestId = 0, frame = 0, ready = false, disposed = false, settled = false;
    let cache = new Map();
    const images = new Map();
    const pendingImages = new Set();
    const charts = new Map();
    let wantedCharts = new Set(), chartRequest = 0;
    let wantedImages = new Set(), fontsReady = false;
    let resolve, reject;
    const completion = new Promise((yes, no) => { resolve = yes; reject = no; });
    const dispose = () => {
        if (disposed) return;
        disposed = true;
        cancelAnimationFrame(frame);
        worker.terminate();
        observer.disconnect();
        signal?.removeEventListener('abort', dispose);
        viewport.removeEventListener('scroll', schedule);
        container.replaceChildren();
        container.classList.remove('workbook');
        cache.clear();
        clearImages();
        clearCharts();
        metadata = rowOffsets = columnOffsets = undefined;
        if (!settled) { settled = true; reject(signal?.reason ?? new DOMException('Preview closed', 'AbortError')); }
    };
    const fail = error => {
        if (disposed) return;
        if (!settled) { settled = true; reject(error); }
        onError?.(error);
        dispose();
    };
    function clearImages() {
        for (const image of images.values()) URL.revokeObjectURL(image.url);
        images.clear();
        pendingImages.clear();
        wantedImages.clear();
    }
    function notifyReady() {
        if (!ready && fontsReady && !disposed && [...wantedImages].every(id => images.has(id)) &&
            [...wantedCharts].every(id => charts.get(id)?.ready)) {
            ready = true;
            onFirstReady?.();
        }
    }
    function imageBounds(range) {
        if (range.absolute) return { ...range.absolute, ...range.ext };
        const x = anchor => (columnOffsets[anchor.nativeCol] ?? columnOffsets.at(-1)) + anchor.nativeColOff / 9525;
        const y = anchor => (rowOffsets[anchor.nativeRow] ?? rowOffsets.at(-1)) + anchor.nativeRowOff / 9525;
        const left = x(range.tl), top = y(range.tl);
        return { left, top, width: range.ext?.width ?? x(range.br) - left, height: range.ext?.height ?? y(range.br) - top };
    }
    function paintImages(fragment) {
        const visible = (metadata.images ?? []).map(image => ({ ...image, bounds: imageBounds(image.range) })).filter(({ bounds }) =>
            bounds.left + bounds.width > viewport.scrollLeft - 100 && bounds.top + bounds.height > viewport.scrollTop - 100 &&
            bounds.left < viewport.scrollLeft + viewport.clientWidth + 100 && bounds.top < viewport.scrollTop + viewport.clientHeight + 100);
        wantedImages = new Set(visible.map(image => image.id));
        for (const [id, image] of images) {
            if (!wantedImages.has(id)) { URL.revokeObjectURL(image.url); images.delete(id); }
        }
        for (const { id, bounds } of visible) {
            const image = images.get(id);
            if (image) {
                const element = document.createElement('img');
                element.className = 'workbook-image';
                element.src = image.url;
                element.alt = '';
                element.style.cssText = `left:${bounds.left}px;top:${bounds.top}px;width:${bounds.width}px;height:${bounds.height}px`;
                fragment.append(element);
            } else if (!pendingImages.has(id) && pendingImages.size < 2) {
                pendingImages.add(id);
                worker.postMessage({ command: 'image', generation, id });
            }
        }
    }
    function releaseChart(chart) {
        chart.controller.abort();
        chart.element.remove();
    }
    function clearCharts() {
        for (const chart of charts.values()) releaseChart(chart);
        charts.clear();
        wantedCharts.clear();
    }
    function paintCharts() {
        const visible = (metadata.charts ?? []).map(chart => ({ ...chart, bounds: imageBounds(chart.range) })).filter(({ bounds }) =>
            bounds.width > 0 && bounds.height > 0 && bounds.left + bounds.width > viewport.scrollLeft - 100 &&
            bounds.top + bounds.height > viewport.scrollTop - 100 && bounds.left < viewport.scrollLeft + viewport.clientWidth + 100 &&
            bounds.top < viewport.scrollTop + viewport.clientHeight + 100);
        wantedCharts = new Set(visible.map(chart => chart.id));
        for (const [id, chart] of charts) {
            if (!wantedCharts.has(id)) { releaseChart(chart); charts.delete(id); }
        }
        for (const { id, bounds } of visible) {
            let chart = charts.get(id);
            if (chart && (chart.width !== bounds.width || chart.height !== bounds.height)) {
                releaseChart(chart);
                charts.delete(id);
                chart = undefined;
            }
            if (!chart) {
                if ([...charts.values()].filter(value => !value.ready).length >= 2) continue;
                const element = document.createElement('div');
                element.className = 'workbook-chart';
                chart = { element, controller: new AbortController(), request: ++chartRequest, width: bounds.width, height: bounds.height, ready: false };
                charts.set(id, chart);
                chartLayer.append(element);
                worker.postMessage({ command: 'chart', generation, id, request: chart.request });
            }
            chart.element.style.cssText = `left:${bounds.left}px;top:${bounds.top}px;width:${bounds.width}px;height:${bounds.height}px`;
        }
    }
    async function receiveChart(data) {
        const chart = charts.get(data.id);
        if (!chart || chart.request !== data.request) return;
        try {
            const { renderWorkbookChart } = await import('./workbook-chart-preview.mjs');
            chart.controller.signal.throwIfAborted();
            await renderWorkbookChart(data.parts, data.theme, chart.element, chart.width, chart.height, chart.controller.signal);
            chart.controller.signal.throwIfAborted();
            chart.ready = true;
            chart.element.dataset.ready = 'true';
            paint();
        } catch (error) {
            if (!disposed && data.generation === generation && charts.get(data.id) === chart && !chart.controller.signal.aborted) fail(error);
        }
    }
    async function receiveImage(data) {
        let url;
        try {
            if (disposed || data.generation !== generation || !wantedImages.has(data.id)) return;
            let imageBytes = data.bytes;
            let extension = data.extension.toLowerCase();
            if (['emf', 'wmf', 'tif', 'tiff'].includes(extension)) {
                if (!convertImage) throw new Error('Native image conversion is unavailable');
                imageBytes = await convertImage(imageBytes, extension, signal);
                extension = 'png';
            }
            const types = { png: 'image/png', jpeg: 'image/jpeg', jpg: 'image/jpeg', gif: 'image/gif', bmp: 'image/bmp', svg: 'image/svg+xml', webp: 'image/webp' };
            if (!types[extension]) throw new Error('Unsupported worksheet image');
            url = URL.createObjectURL(new Blob([imageBytes], { type: types[extension] }));
            const probe = new Image();
            probe.src = url;
            await probe.decode();
            if (disposed || data.generation !== generation || !wantedImages.has(data.id)) return;
            images.set(data.id, { url, probe });
            url = undefined;
        } catch (error) {
            if (!disposed && data.generation === generation && wantedImages.has(data.id)) fail(error);
        } finally {
            if (url) URL.revokeObjectURL(url);
            if (!disposed && data.generation === generation) { pendingImages.delete(data.id); paint(); }
        }
    }
    function setMetadata(value) {
        metadata = value;
        rowOffsets = axis(value.rows, value.defaultRowHeight, value.rowSettings, height => Math.max(0, height * 4 / 3));
        columnOffsets = axis(value.columns, value.defaultColumnWidth, value.columnSettings, width => Math.max(0, Math.round(width * 7 + 5)));
        stage.style.height = `${rowOffsets.at(-1)}px`;
        stage.style.width = `${columnOffsets.at(-1)}px`;
    }
    function paint() {
        if (!metadata || disposed) return;
        const rows = visibleIndices(rowOffsets, viewport.scrollTop, viewport.clientHeight, 256);
        const columns = visibleIndices(columnOffsets, viewport.scrollLeft, viewport.clientWidth, 128);
        const cells = document.createDocumentFragment();
        const rowLabels = document.createDocumentFragment();
        const columnLabels = document.createDocumentFragment();
        const mergeCells = metadata.merges.filter(merge => merge.bottom >= rows[0] && merge.top <= rows.at(-1) && merge.right >= columns[0] && merge.left <= columns.at(-1));
        function addCell(row, column, data, bottom = row, right = column) {
            const element = document.createElement('div');
            element.className = 'workbook-cell';
            element.dataset.address = `${columnName(column)}${row}`;
            element.style.cssText = `left:${columnOffsets[column - 1]}px;top:${rowOffsets[row - 1]}px;width:${columnOffsets[right] - columnOffsets[column - 1]}px;height:${rowOffsets[bottom] - rowOffsets[row - 1]}px`;
            populateCell(element, data, metadata);
            cells.append(element);
        }
        for (const row of rows) {
            const label = document.createElement('div');
            label.className = 'workbook-header';
            label.textContent = row;
            label.style.cssText = `top:${rowOffsets[row - 1] - viewport.scrollTop}px;height:${rowOffsets[row] - rowOffsets[row - 1]}px;width:100%`;
            rowLabels.append(label);
            const values = new Map((cache.get(row)?.cells ?? []).map(cell => [cell.column, cell]));
            for (const column of columns) {
                if (mergeCells.some(merge => row >= merge.top && row <= merge.bottom && column >= merge.left && column <= merge.right)) continue;
                addCell(row, column, values.get(column));
            }
        }
        for (const merge of mergeCells) addCell(merge.top, merge.left, merge.cell, Math.min(merge.bottom, metadata.rows), Math.min(merge.right, metadata.columns));
        for (const column of columns) {
            const label = document.createElement('div');
            label.className = 'workbook-header';
            label.textContent = columnName(column);
            label.style.cssText = `left:${columnOffsets[column - 1] - viewport.scrollLeft}px;width:${columnOffsets[column] - columnOffsets[column - 1]}px;height:100%`;
            columnLabels.append(label);
        }
        paintImages(cells);
        cellLayer.replaceChildren(cells);
        paintCharts();
        rowHeaders.replaceChildren(rowLabels);
        columnHeaders.replaceChildren(columnLabels);
        notifyReady();
        return { rows, columns };
    }
    function refresh() {
        frame = 0;
        const indices = paint();
        if (indices) worker.postMessage({ command: 'range', generation, id: ++requestId, ...indices });
    }
    function schedule() { if (!frame && !disposed) frame = requestAnimationFrame(refresh); }
    const observer = new ResizeObserver(schedule);
    observer.observe(viewport);
    viewport.addEventListener('scroll', schedule, { passive: true });
    signal?.addEventListener('abort', dispose, { once: true });
    worker.onerror = event => fail(new Error(event.message || 'Workbook worker failed'));
    worker.onmessage = ({ data }) => {
        if (disposed || data.generation !== generation) return;
        try {
            if (data.type === 'error') { fail(new Error(data.message)); return; }
            if (data.type === 'first') {
                cache = new Map(data.rows.map(row => [row.index, row]));
                setMetadata(data.metadata);
                viewport.scrollTo(0, 0);
                tabs.replaceChildren(...data.sheets.flatMap((sheet, index) => {
                    if (sheet.hidden) return [];
                    const button = document.createElement('button');
                    button.type = 'button';
                    button.role = 'tab';
                    button.textContent = sheet.name;
                    button.setAttribute('aria-selected', String(index === data.index));
                    button.onclick = () => {
                        if (button.getAttribute('aria-selected') === 'true') return;
                        ++generation;
                        ++requestId;
                        metadata = undefined;
                        cache.clear();
                        clearImages();
                        clearCharts();
                        cellLayer.replaceChildren();
                        for (const tab of tabs.children) tab.setAttribute('aria-selected', String(tab === button));
                        worker.postMessage({ command: 'select', index });
                    };
                    return [button];
                }));
                paint();
                const firstGeneration = generation;
                void document.fonts.ready.then(() => {
                    if (!disposed && firstGeneration === generation) { fontsReady = true; notifyReady(); }
                }).catch(fail);
            } else if (data.type === 'complete') {
                setMetadata(data.metadata);
                refresh();
                if (!settled) { settled = true; resolve({ dispose }); }
            } else if (data.type === 'range' && data.id === requestId) {
                cache = new Map(data.rows.map(row => [row.index, row]));
                paint();
            } else if (data.type === 'image') {
                void receiveImage(data);
            } else if (data.type === 'chart') {
                void receiveChart(data);
            }
        } catch (error) { fail(error); }
    };
    try {
        const owned = bytes.slice(0);
        worker.postMessage({ command: 'open', bytes: owned }, [owned]);
        onSourceConsumed?.();
    } catch (error) { fail(error); }
    return completion;
}
