import { SaxesParser } from 'saxes';
import { resolveWorkbookPart } from './workbook-archive.js';

const drawingNamespace = 'http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing';
const chartNamespace = 'http://schemas.openxmlformats.org/drawingml/2006/chart';
const relationshipNamespace = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
const packageNamespace = 'http://schemas.openxmlformats.org/package/2006/relationships';

function attribute(tag, name, uri = '') {
    return Object.values(tag.attributes).find(value => value.local === name && value.uri === uri)?.value;
}

function parser() {
    const sax = new SaxesParser({ xmlns: true });
    sax.on('doctype', () => { throw new Error('Document types are not supported'); });
    return sax;
}

async function relationships(zip, part, suffix) {
    const separator = part.lastIndexOf('/');
    const file = zip.file(`${part.slice(0, separator + 1)}_rels/${part.slice(separator + 1)}.rels`);
    const result = new Map();
    if (!file) return result;
    const sax = parser();
    sax.on('opentag', tag => {
        if (tag.uri !== packageNamespace || tag.local !== 'Relationship' || attribute(tag, 'Type') !== `${relationshipNamespace}/${suffix}`) return;
        const id = attribute(tag, 'Id');
        if (!id || result.has(id) || attribute(tag, 'TargetMode') === 'External') throw new Error('Invalid drawing relationship');
        result.set(id, resolveWorkbookPart(part, attribute(tag, 'Target')));
    });
    sax.write(await file.async('string')).close();
    return result;
}

export function readChartAnchors(xml, links) {
    const result = [];
    const sax = parser();
    const stack = [];
    let anchor;
    sax.on('opentag', tag => {
        stack.push({ tag, text: '' });
        if (tag.uri === drawingNamespace && ['oneCellAnchor', 'twoCellAnchor', 'absoluteAnchor'].includes(tag.local)) {
            if (anchor) throw new Error('Nested drawing anchor');
            anchor = { depth: stack.length, from: {}, to: {} };
        }
        if (!anchor) return;
        if (tag.uri === chartNamespace && tag.local === 'chart') {
            const id = attribute(tag, 'id', relationshipNamespace);
            if (anchor.path || !links.has(id)) throw new Error('Invalid chart reference');
            anchor.path = links.get(id);
        }
        if (tag.uri === drawingNamespace && tag.local === 'ext' && stack.length === anchor.depth + 1) {
            anchor.ext = { width: Number(attribute(tag, 'cx')) / 9525, height: Number(attribute(tag, 'cy')) / 9525 };
        }
        if (tag.uri === drawingNamespace && tag.local === 'pos' && stack.length === anchor.depth + 1) {
            anchor.absolute = { left: Number(attribute(tag, 'x')) / 9525, top: Number(attribute(tag, 'y')) / 9525 };
        }
    });
    sax.on('text', text => { if (stack.length) stack.at(-1).text += text; });
    sax.on('closetag', tag => {
        const item = stack.pop();
        const parent = stack.at(-1)?.tag;
        if (!anchor) return;
        if (tag.uri === drawingNamespace && ['col', 'colOff', 'row', 'rowOff'].includes(tag.local) &&
            parent?.uri === drawingNamespace && ['from', 'to'].includes(parent.local)) {
            const value = Number(item.text);
            if (!Number.isSafeInteger(value) || value < 0) throw new Error('Invalid chart coordinate');
            anchor[parent.local][tag.local] = value;
        }
        if (stack.length + 1 !== anchor.depth) return;
        if (anchor.path) {
            const convert = value => ({ nativeCol: value.col, nativeColOff: value.colOff ?? 0, nativeRow: value.row, nativeRowOff: value.rowOff ?? 0 });
            const range = { tl: convert(anchor.from), br: anchor.to.col === undefined ? undefined : convert(anchor.to), ext: anchor.ext, absolute: anchor.absolute };
            if (range.absolute) {
                if (![range.absolute.left, range.absolute.top].every(value => Number.isFinite(value) && value >= 0)) throw new Error('Invalid absolute chart position');
            } else if (![range.tl.nativeCol, range.tl.nativeRow].every(Number.isSafeInteger)) throw new Error('Chart origin is missing');
            if (!range.br && !range.ext) throw new Error('Chart size is missing');
            if (range.ext && ![range.ext.width, range.ext.height].every(value => Number.isFinite(value) && value > 0 && value <= 1000000)) throw new Error('Invalid chart size');
            result.push({ path: anchor.path, range });
            if (result.length > 1000) throw new Error('Too many worksheet charts');
        }
        anchor = undefined;
    });
    sax.write(xml).close();
    return result;
}

export async function workbookCharts(archive, index, drawingIds) {
    if (!drawingIds.length) return [];
    const links = await relationships(archive.zip, archive.sheets[index].target, 'drawing');
    const charts = [];
    for (const id of drawingIds) {
        const part = links.get(id);
        const file = part && archive.zip.file(part);
        if (!file) throw new Error('Worksheet drawing is missing');
        const chartLinks = await relationships(archive.zip, part, 'chart');
        if (!chartLinks.size) continue;
        charts.push(...readChartAnchors(await file.async('string'), chartLinks));
    }
    if (charts.length > 1000) throw new Error('Too many worksheet charts');
    return charts.map((chart, id) => ({ ...chart, id }));
}

export async function workbookChartParts(archive, chart) {
    const file = archive.zip.file(chart.path);
    if (!file) throw new Error('Chart part is missing');
    const separator = chart.path.lastIndexOf('/');
    const relPath = `${chart.path.slice(0, separator + 1)}_rels/${chart.path.slice(separator + 1)}.rels`;
    const rels = await archive.zip.file(relPath)?.async('string');
    const result = { path: chart.path, xml: await file.async('string'), relPath, rels, styles: [], colors: [], themes: [] };
    if (!rels) return result;
    const pending = [];
    const sax = parser();
    sax.on('opentag', tag => {
        if (tag.uri !== packageNamespace || tag.local !== 'Relationship') return;
        const kind = attribute(tag, 'Type')?.split('/').at(-1);
        const field = { chartStyle: 'styles', chartColorStyle: 'colors', themeOverride: 'themes' }[kind];
        if (!field) return;
        if (attribute(tag, 'TargetMode') === 'External') throw new Error('External chart styles are unsupported');
        const path = resolveWorkbookPart(chart.path, attribute(tag, 'Target'));
        const part = archive.zip.file(path);
        if (!part) throw new Error('Chart style is missing');
        pending.push(part.async('string').then(xml => result[field].push([path, xml])));
    });
    sax.write(rels).close();
    await Promise.all(pending);
    return result;
}
