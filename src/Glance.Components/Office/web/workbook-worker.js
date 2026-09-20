import ExcelJS from 'exceljs';
import { openWorkbookArchive, projectWorksheet, workbookThemeColors } from './workbook-archive.js';
import { workbookCharts, workbookChartParts } from './workbook-drawings.js';

let archive;
let requested;
let running = false;
let generation = 0;
let current;
// Input validation rules are editor-only and may expand entire column ranges into per-cell objects.
const readOptions = { ignoreNodes: ['dataValidations'] };

async function parseWorkbook(bytes) {
    const reader = new ExcelJS.Workbook();
    let model;
    // The pinned parser has already reconciled styles, strings, dates and drawings here.
    Object.defineProperty(reader, 'model', { set(value) { model = value; } });
    await reader.xlsx.load(bytes, readOptions);
    if (!Array.isArray(model?.worksheets)) throw new Error('Worksheet parsing failed');
    return model;
}

function address(value) {
    const match = /^([A-Z]+)([1-9][0-9]*)$/.exec(value);
    if (!match) throw new Error('Invalid worksheet address');
    const column = [...match[1]].reduce((number, char) => number * 26 + char.charCodeAt(0) - 64, 0);
    const row = Number(match[2]);
    if (column > 16384 || row > 1048576) throw new Error('Invalid worksheet address');
    return { row, column };
}

function snapshot(workbook, sheet, source, charts, projection) {
    const rows = [], rowSettings = [], columnSettings = [], rowStyles = [], columnStyles = [];
    let columns = 0;
    for (const column of sheet.cols ?? []) {
        if (column.min < 1 || column.max > 16384 || column.min > column.max) throw new Error('Invalid worksheet columns');
        for (let index = column.min; index <= column.max; ++index) {
            columnSettings.push({ index, width: column.width, hidden: column.hidden });
            columnStyles[index - 1] = column.style;
        }
    }
    for (const row of sheet.rows) {
        if (!Number.isSafeInteger(row.number) || row.number < 1 || row.number > 1048576) throw new Error('Invalid worksheet row');
        if (row.height !== undefined || row.hidden) rowSettings.push({ index: row.number, height: row.height, hidden: row.hidden });
        rowStyles[row.number - 1] = row.style;
        const cells = rows[row.number - 1] = [];
        for (const cell of row.cells) {
            const position = address(cell.address);
            if (position.row !== row.number) throw new Error('Mismatched worksheet cell row');
            cells[position.column - 1] = cell;
            cell.style ??= {};
            columns = Math.max(columns, position.column);
        }
    }
    const merges = [];
    let mergedCells = 0;
    for (const reference of sheet.mergeCells ?? []) {
        const parts = reference.split(':');
        const first = address(parts[0]), last = address(parts[1] ?? parts[0]);
        if (first.row > last.row || first.column > last.column) throw new Error('Invalid worksheet merge');
        mergedCells += (last.row - first.row + 1) * (last.column - first.column + 1);
        if (mergedCells > 1000000) throw new Error('Worksheet exceeds merged cell budget');
        for (let row = first.row; row <= last.row; ++row) for (let column = first.column; column <= last.column; ++column) {
            const cells = rows[row - 1] ??= [];
            let cell = cells[column - 1];
            if (!cell || cell.type === 1) {
                let name = '', number = column;
                while (number) { --number; name = String.fromCharCode(65 + number % 26) + name; number = Math.floor(number / 26); }
                const style = cell?.style ?? {};
                for (const key of ['numFmt', 'font', 'alignment', 'border', 'fill', 'protection']) {
                    style[key] ??= rowStyles[row - 1]?.[key] ?? columnStyles[column - 1]?.[key];
                    if (style[key] === undefined) delete style[key];
                }
                cell = cells[column - 1] = { address: name + row, type: 0, style };
            }
            if (row !== first.row || column !== first.column) cells[column - 1] = {
                address: cell.address, type: 1, master: parts[0], style: cell.style,
            };
        }
        columns = Math.max(columns, last.column);
        merges.push({ top: first.row, left: first.column, bottom: last.row, right: last.column, cell: rows[first.row - 1][first.column - 1] });
    }
    const details = metadata(workbook, sheet, projection, charts, { rows: rows.length, columns, rowSettings, columnSettings, merges });
    const media = new Map(details.images.map(image => [image.id, workbook.media[image.id]]));
    return { rows, metadata: details, media, theme: workbook.themes?.theme1, generation, source, charts };
}

function range(data, indices, columns) {
    const rows = [];
    for (const index of indices) {
        const row = data[index - 1];
        const cells = [];
        for (const column of columns) {
            const cell = row?.[column - 1];
            if (cell) cells.push({ column, ...cell });
        }
        rows.push({ index, cells });
    }
    return rows;
}

function metadata(workbook, sheet, projection, charts, details) {
    const images = (sheet.media ?? []).filter(image => image.type === 'image').map(image => ({ id: image.imageId, range: image.range }));
    let rows = Math.max(projection?.rows ?? 0, details.rows);
    let columns = Math.max(projection?.columns ?? 0, details.columns);
    for (const { range } of [...images, ...charts]) {
        const validAnchor = anchor => anchor && [anchor.nativeCol, anchor.nativeRow, anchor.nativeColOff, anchor.nativeRowOff]
            .every(value => Number.isSafeInteger(value) && value >= 0);
        if ((!range.absolute && !validAnchor(range.tl)) || (range.br && !validAnchor(range.br)) ||
            (!range.br && !range.ext) || (range.ext && ![range.ext.width, range.ext.height].every(value => Number.isFinite(value) && value > 0 && value <= 1000000))) {
            throw new Error('Invalid worksheet image anchor');
        }
        const row = range.absolute ? Math.ceil(range.absolute.top / 20) : range.tl.nativeRow;
        const column = range.absolute ? Math.ceil(range.absolute.left / 64) : range.tl.nativeCol;
        rows = Math.max(rows, (range.br?.nativeRow ?? row + Math.ceil((range.ext?.height ?? 0) / 20)) + 1);
        columns = Math.max(columns, (range.br?.nativeCol ?? column + Math.ceil((range.ext?.width ?? 0) / 64)) + 1);
    }
    if (rows > 1048576 || columns > 16384 || images.length > 1000) throw new Error('Worksheet drawing exceeds preview budget');
    return {
        rows,
        columns,
        images,
        charts: charts.map(({ id, range }) => ({ id, range })),
        theme: workbookThemeColors(workbook.themes?.theme1),
        defaultRowHeight: sheet.properties.defaultRowHeight ?? 15,
        defaultColumnWidth: sheet.properties.defaultColWidth ?? 8.43,
        rowSettings: projection?.rowSettings ?? details.rowSettings,
        columnSettings: details.columnSettings,
        merges: details.merges,
        date1904: workbook.properties.date1904 === true,
    };
}

async function pump() {
    if (running) return;
    running = true;
    try {
        while (requested) {
            const request = requested;
            requested = undefined;
            try {
                const source = await archive;
                if (request.generation !== generation) continue;
                const index = request.index ?? source.activeTab;
                if (!Number.isSafeInteger(index) || !source.sheets[index] || source.sheets[index].hidden) {
                    throw new RangeError('Invalid worksheet selection');
                }
                current = undefined;
                const first = await projectWorksheet(source, index, 60);
                const charts = await workbookCharts(source, index, first.drawingIds);
                if (request.generation !== generation) continue;
                let workbook = await parseWorkbook(first.bytes);
                if (request.generation !== generation) continue;
                let sheet = workbook.worksheets.find(sheet => sheet.name === source.sheets[index].name);
                if (!sheet) throw new Error('Worksheet parsing failed');
                current = snapshot(workbook, sheet, source, charts, first);
                postMessage({
                    type: 'first', generation, index,
                    sheets: source.sheets.map(({ name, hidden }) => ({ name, hidden })),
                    metadata: current.metadata,
                    rows: range(current.rows, Array.from({ length: Math.min(60, current.rows.length) }, (_, index) => index + 1),
                        Array.from({ length: Math.min(128, current.metadata.columns) }, (_, index) => index + 1)),
                });
                if (first.truncated) {
                    workbook = sheet = undefined;
                    const complete = await projectWorksheet(source, index);
                    if (request.generation !== generation) continue;
                    workbook = await parseWorkbook(complete.bytes);
                    if (request.generation !== generation) continue;
                    sheet = workbook.worksheets.find(sheet => sheet.name === source.sheets[index].name);
                    if (!sheet) throw new Error('Worksheet parsing failed');
                    current = snapshot(workbook, sheet, source, charts);
                }
                workbook = sheet = undefined;
                postMessage({ type: 'complete', generation, metadata: current.metadata });
            } catch (error) {
                if (request.generation === generation) postMessage({ type: 'error', generation, message: String(error) });
            }
        }
    } finally { running = false; }
}

self.onmessage = ({ data }) => {
    try {
        if (data.command === 'open') {
            if (archive) throw new Error('Workbook is already open');
            archive = openWorkbookArchive(data.bytes);
            archive.catch(() => {});
            requested = { generation: ++generation };
            void pump();
        } else if (data.command === 'select') {
            if (!archive) throw new Error('Workbook is not open');
            requested = { index: data.index, generation: ++generation };
            void pump();
        } else if (data.command === 'range') {
            if (!current || data.generation !== current.generation || data.generation !== generation) return;
            const valid = (values, count, maximum) => Array.isArray(values) && values.length <= count &&
                values.every(value => Number.isSafeInteger(value) && value >= 1 && value <= maximum);
            if (!valid(data.rows, 256, 1048576) || !valid(data.columns, 128, 16384)) {
                throw new RangeError('Invalid worksheet range');
            }
            postMessage({
                type: 'range', generation, id: data.id,
                rows: range(current.rows, data.rows, data.columns),
            });
        } else if (data.command === 'image') {
            if (!current || data.generation !== current.generation || data.generation !== generation) return;
            if (!Number.isSafeInteger(data.id) || !current.media.has(data.id)) {
                throw new RangeError('Invalid worksheet image');
            }
            const media = current.media.get(data.id);
            if (!media?.buffer || media.buffer.byteLength > 8 * 1024 * 1024) throw new Error('Worksheet image exceeds preview budget');
            const bytes = new Uint8Array(media.buffer).slice();
            postMessage({ type: 'image', generation, id: data.id, extension: media.extension, bytes }, [bytes.buffer]);
        } else if (data.command === 'chart') {
            if (!current || data.generation !== generation || current.generation !== generation) return;
            const owner = current;
            const chart = Number.isSafeInteger(data.id) && owner.charts.find(chart => chart.id === data.id);
            if (!chart) throw new RangeError('Invalid worksheet chart');
            void workbookChartParts(owner.source, chart).then(parts => {
                if (data.generation !== generation) return;
                postMessage({ type: 'chart', generation, id: data.id, request: data.request, parts, theme: owner.theme });
            }).catch(error => {
                if (data.generation === generation) postMessage({ type: 'error', generation, message: String(error) });
            });
        }
    } catch (error) { postMessage({ type: 'error', generation, message: String(error) }); }
};
