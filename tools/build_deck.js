// Generates docs/myOS-arquitectura.pptx
const pptxgen = require("pptxgenjs");
const { applyTheme } = require("/Users/jorgesaymar/.claude/skills/synced/7782838f-2702-44d0-b773-732e59e30c79_2bff71bb-6966-4ac2-807c-796a7ec492ab/pptx/scripts/apply_theme.js");

const OUT = "/Users/jorgesaymar/Desktop/myOS/docs/myOS-arquitectura.pptx";
const IMG = "/Users/jorgesaymar/Desktop/myOS/docs/img/";

const THEME = {
  name: "myOS",
  headFontFace: "Calibri",
  bodyFontFace: "Calibri",
  colors: {
    dk1: "1D1D1F", lt1: "FFFFFF", dk2: "1E1B4B", lt2: "F1F0FA",
    accent1: "6D4AFF", accent2: "FF5FA2", accent3: "007AFF",
    accent4: "28C840", accent5: "FEBC2E", accent6: "FF5F57",
    hlink: "007AFF", folHlink: "6D4AFF",
  },
};
const HEX = THEME.colors;

const pres = new pptxgen();
pres.layout = "LAYOUT_16x9"; // 10 x 5.625
pres.title = "myOS: arquitectura y pasos de construcción";
pres.author = "myOS";
pres.theme = { headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace };
const C = pres.SchemeColor;

// ---------------------------------------------------------------- layouts
pres.defineSlideMaster({
  title: "DARK_TITLE",
  background: { color: C.text2 },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 1.35, w: 4.4, h: 1.1,
        fontSize: 54, bold: true, color: C.background1, valign: "bottom", align: "left", margin: 0 }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: 0.6, y: 2.55, w: 4.4, h: 1.2,
        fontSize: 20, color: C.background2, valign: "top", align: "left", margin: 0 }, text: "" } },
  ],
});
pres.defineSlideMaster({
  title: "DARK_SECTION",
  background: { color: C.text2 },
  objects: [
    { placeholder: { options: { name: "kicker", type: "body", x: 0.8, y: 1.6, w: 8.4, h: 0.6,
        fontSize: 20, color: C.accent2, bold: true, align: "left", margin: 0 }, text: "" } },
    { placeholder: { options: { name: "title", type: "title", x: 0.8, y: 2.2, w: 8.4, h: 1.0,
        fontSize: 44, bold: true, color: C.background1, align: "left", margin: 0 }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: 0.8, y: 3.25, w: 8.4, h: 0.8,
        fontSize: 18, color: C.background2, align: "left", margin: 0 }, text: "" } },
  ],
});
pres.defineSlideMaster({
  title: "CONTENT",
  background: { color: C.background1 },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: 0.5, y: 0.28, w: 9.0, h: 0.7,
        fontSize: 30, bold: true, color: C.text2, valign: "middle", margin: 0 }, text: "" } },
  ],
  slideNumber: { x: 9.1, y: 5.22, w: 0.5, h: 0.25, fontSize: 10, color: "86868B", align: "right" },
});

// ---------------------------------------------------------------- helpers
const shadow = () => ({ type: "outer", color: "1E1B4B", opacity: 0.18, blur: 8, offset: 2, angle: 90 });

// The deck's motif: every card is a little myOS window with traffic lights.
function lights(slide, x, y, name) {
  [C.accent6, C.accent5, C.accent4].forEach((c, i) =>
    slide.addShape(pres.shapes.OVAL, { x: x + i * 0.17, y, w: 0.11, h: 0.11, fill: { color: c },
      line: { color: c, width: 0 }, objectName: `${name}-light${i}` }));
}
function windowCard(slide, x, y, w, h, title, name, opts = {}) {
  slide.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w, h, rectRadius: 0.08,
    fill: { color: opts.fill || C.background1 }, line: { color: "D6D6DC", width: 0.75 },
    shadow: shadow(), objectName: `${name}-card` });
  lights(slide, x + 0.15, y + 0.14, name);
  if (title) slide.addText(title, { x: x + 0.15, y: y + 0.3, w: w - 0.3, h: 0.36, fontSize: opts.titleSize || 15,
    bold: true, color: opts.titleColor || C.text2, margin: 0, isTextBox: true, objectName: `${name}-title`,
    valign: "middle" });
}
function body(slide, x, y, w, h, items, name, size = 13, color = C.text1) {
  slide.addText(items.map((t, i) => ({ text: t, options: { bullet: true, breakLine: i < items.length - 1 } })),
    { x, y, w, h, fontSize: size, color, margin: 0, paraSpaceAfter: 4, valign: "top", isTextBox: true,
      objectName: name });
}

// ---------------------------------------------------------------- 1. title
pres.addSection({ title: "Introducción" });
let s = pres.addSlide({ masterName: "DARK_TITLE", sectionTitle: "Introducción" });
s.addText("myOS", { placeholder: "title" });
s.addText("Cómo se construyó un sistema operativo desde cero", { placeholder: "body" });
s.addText("x86_64 · C + NASM · Limine · QEMU · Claude Code multi-agente", { x: 0.6, y: 4.55, w: 4.6, h: 0.35,
  fontSize: 12, color: C.accent2, margin: 0, isTextBox: true, objectName: "tagline" });
s.addImage({ path: IMG + "desktop-v0.1-macos.png", x: 5.2, y: 1.3, w: 4.3, h: 2.6875, rounding: false,
  shadow: { type: "outer", color: "000000", opacity: 0.45, blur: 14, offset: 4, angle: 90 }, objectName: "hero",
  altText: "Escritorio de myOS v0.1 con estilo macOS" });
s.addNotes("myOS es un sistema operativo hobby escrito desde cero en C y ensamblador para x86_64. Arranca con Limine y corre en QEMU en un Mac con Apple Silicon. Esta presentación explica su arquitectura y los pasos con los que se construyó usando varios agentes de Claude Code en paralelo.");

// ---------------------------------------------------------------- 2. video vs myOS
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Introducción" });
s.addText("Partimos de un video y cambiamos el método", { placeholder: "title" });
windowCard(s, 0.5, 1.2, 4.35, 3.75, "AuroraOS (el video)", "video");
body(s, 0.68, 1.98, 4.0, 2.9, [
  "Dos prompts gigantes, todo de golpe",
  "GRUB + BIOS, pensado para VirtualBox",
  "No arrancó en un Mac ARM",
  "Verificación a ojo",
  "HTTPS resuelto por un programa en el Mac",
  "Todo dentro del kernel; nota final 6,5/10",
], "video-list", 14);
windowCard(s, 5.15, 1.2, 4.35, 3.75, "myOS (nuestro enfoque)", "myos", { titleColor: C.accent1 });
body(s, 5.33, 1.98, 4.0, 2.9, [
  "16 fases pequeñas, una a una",
  "Limine: BIOS y UEFI, 64 bits desde el arranque",
  "QEMU como plataforma principal",
  "make test automático en cada paso",
  "Red TCP/IP propia dentro del OS",
  "Procesos aislados en ring 3; 7 agentes en paralelo",
], "myos-list", 14);
s.addNotes("El punto de partida fue el video de The Render Guy, donde Claude generó AuroraOS con dos prompts. Funcionó, pero no arrancaba en un Mac ARM, se verificaba a ojo y el HTTPS dependía del Mac. myOS cambia el método: fases pequeñas, tests automáticos, Limine y QEMU, y varios agentes trabajando en paralelo con contratos claros.");

// ---------------------------------------------------------------- 3. numbers
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Introducción" });
s.addText("myOS hoy, en cinco números", { placeholder: "title" });
const stats = [
  ["2,8 s", "del encendido al escritorio"],
  ["~10.000", "líneas de C y ensamblador"],
  ["7", "agentes de Claude en paralelo"],
  ["23", "autotests [OK] en cada arranque"],
  ["75 %", "del plan completado (12 de 16 fases)"],
];
stats.forEach(([n, l], i) => {
  const x = 0.5 + i * 1.84;
  windowCard(s, x, 1.5, 1.66, 2.6, null, `stat${i}`);
  s.addText(n, { x: x + 0.08, y: 1.95, w: 1.5, h: 0.9, fontSize: 34, bold: true, color: C.accent1, align: "center",
    margin: 0, isTextBox: true, objectName: `stat${i}-n`, fit: "shrink" });
  s.addText(l, { x: x + 0.12, y: 2.9, w: 1.42, h: 0.9, fontSize: 12, color: C.text1, align: "center",
    valign: "top", margin: 0, isTextBox: true, objectName: `stat${i}-l` });
});
s.addText("Medido en un Mac M3 emulando x86_64 con QEMU (máquina q35, 256 MiB).", { x: 0.5, y: 4.4, w: 9, h: 0.3,
  fontSize: 11, italic: true, color: "86868B", margin: 0, isTextBox: true, objectName: "stats-note" });
s.addNotes("El sistema arranca en unos 2,8 segundos hasta mostrar el escritorio. Son unas diez mil líneas de C y ensamblador, construidas por siete agentes en dos rondas. Cada arranque ejecuta 23 autotests y el plan está al 75 por ciento.");

// ---------------------------------------------------------------- 4. section
pres.addSection({ title: "Arquitectura" });
s = pres.addSlide({ masterName: "DARK_SECTION", sectionTitle: "Arquitectura" });
s.addText("01", { placeholder: "kicker" });
s.addText("Arquitectura", { placeholder: "title" });
s.addText("Del encendido al escritorio: capas, contratos y memoria", { placeholder: "body" });
s.addNotes("Primera parte: cómo está organizado el sistema por dentro.");

// ---------------------------------------------------------------- 5. boot flow
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Arquitectura" });
s.addText("Del encendido al escritorio en 2,8 segundos", { placeholder: "title" });
const boot = [
  ["0,8 s", "Firmware + Limine", "Carga el kernel en la mitad alta"],
  ["0,85 s", "arch + mm", "Interrupciones, timer, memoria"],
  ["1,1 s", "sched + ring 3", "Hilos y procesos aislados"],
  ["1,1 s", "input + PCI", "Teclado, ratón, reloj, bus"],
  ["1,4 s", "fs", "Disco SATA con FAT32"],
  ["2,7 s", "net", "DHCP, DNS, TCP, HTTP"],
  ["2,8 s", "desktop", "Escritorio visible"],
];
s.addShape(pres.shapes.LINE, { x: 0.85, y: 2.45, w: 8.3, h: 0, line: { color: "C9C3F5", width: 2 },
  objectName: "boot-line" });
boot.forEach(([t, name, desc], i) => {
  const cx = 0.85 + i * (8.3 / 6);
  const col = i === boot.length - 1 ? C.accent2 : C.accent1;
  s.addText(t, { x: cx - 0.6, y: 1.55, w: 1.2, h: 0.4, fontSize: 16, bold: true, color: col, align: "center",
    margin: 0, isTextBox: true, objectName: `boot${i}-t` });
  s.addShape(pres.shapes.OVAL, { x: cx - 0.17, y: 2.28, w: 0.34, h: 0.34, fill: { color: col },
    line: { color: C.background1, width: 2 }, objectName: `boot${i}-dot` });
  s.addText(name, { x: cx - 0.65, y: 2.85, w: 1.3, h: 0.4, fontSize: 14, bold: true, color: C.text2,
    align: "center", margin: 0, isTextBox: true, objectName: `boot${i}-name` });
  s.addText(desc, { x: cx - 0.65, y: 3.25, w: 1.3, h: 0.75, fontSize: 11, color: C.text1, align: "center",
    valign: "top", margin: 0, isTextBox: true, objectName: `boot${i}-desc` });
});
s.addText("kmain() solo llama a cada *_init en orden; cada módulo se autoverifica e imprime [OK] o detiene el arranque con panic().",
  { x: 0.5, y: 4.35, w: 9, h: 0.55, fontSize: 13, color: C.text2, margin: 0, isTextBox: true, objectName: "boot-note",
    italic: true });
s.addNotes("Al encender, el firmware carga Limine, que pone la CPU en 64 bits y salta a kmain. kmain es muy fino: llama a los init de cada subsistema en orden. Casi la mitad del tiempo se va en la prueba de red, que sale a Internet; se puede limitar al modo test para bajar a 1,5 segundos.");

// ---------------------------------------------------------------- 6. layers
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Arquitectura" });
s.addText("Seis capas, una carpeta por subsistema", { placeholder: "title" });
const layers = [
  ["Apps", "Welcome · Notes · Clock", "kernel/apps", C.accent2, 55],
  ["Escritorio", "Gráficos · compositor por zonas · ventanas · barra y dock", "kernel/gfx · wm", C.accent2, 75],
  ["Servicios", "Hilos y ring 3  ·  VFS + FAT32  ·  TCP/IP, DHCP, DNS, HTTP", "kernel/proc · fs · net", C.accent1, 60],
  ["Núcleo", "GDT/IDT, PIC, timer 1000 Hz  ·  memoria física, paginación, heap", "kernel/arch · mm", C.accent1, 78],
  ["Drivers", "Serie · PS/2 teclado y ratón · RTC · PCI · AHCI · e1000", "kernel/drivers", C.accent3, 70],
  ["Hardware", "QEMU q35: CPU x86_64, 256 MiB, disco SATA, tarjeta e1000", "emulado en el Mac", C.text2, 82],
];
layers.forEach(([name, desc, path, col, tr], i) => {
  const y = 1.15 + i * 0.63;
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y, w: 9.0, h: 0.54, rectRadius: 0.08,
    fill: { color: col, transparency: tr }, line: { color: col, width: 0.75, transparency: 40 },
    objectName: `layer${i}` });
  s.addText(name, { x: 0.7, y, w: 1.5, h: 0.54, fontSize: 15, bold: true, color: C.text2, valign: "middle",
    margin: 0, isTextBox: true, objectName: `layer${i}-name` });
  s.addText(desc, { x: 2.2, y, w: 5.2, h: 0.54, fontSize: 13, color: C.text1, valign: "middle", margin: 0,
    isTextBox: true, objectName: `layer${i}-desc` });
  s.addText(path, { x: 7.4, y, w: 1.95, h: 0.54, fontSize: 11, color: C.text2, valign: "middle", align: "right",
    fontFace: "Courier New", margin: 0, isTextBox: true, objectName: `layer${i}-path` });
});
s.addNotes("La arquitectura tiene seis capas. Abajo, el hardware que emula QEMU. Encima, los drivers. Luego el núcleo: CPU, interrupciones y memoria. Después los servicios: multitarea, archivos y red. Arriba, el escritorio y las apps. Cada capa vive en su carpeta, lo que permitió asignar una carpeta a cada agente.");

// ---------------------------------------------------------------- 7. contracts
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Arquitectura" });
s.addText("Contratos + reservas: así se trabaja en paralelo", { placeholder: "title" });
const steps = [
  ["1. Contrato", "kernel/include/mm.h", "void *kmalloc(size_t n);", "Declara qué ofrece el módulo. Lo escribe el integrador antes de lanzar agentes."],
  ["2. Reserva débil", "kernel/lib/weak_stubs.c", "WEAK void *kmalloc(...)\n  { /* bloque fijo */ }", "Versión mínima para que todo compile y arranque sin el módulo real."],
  ["3. Implementación real", "kernel/mm/heap.c", "void *kmalloc(size_t n)\n  { /* heap real */ }", "La escribe el agente. Al enlazar, la versión fuerte sustituye a la débil."],
];
steps.forEach(([t, file, code, desc], i) => {
  const x = 0.5 + i * 3.1;
  windowCard(s, x, 1.15, 2.8, 3.55, t, `contract${i}`, { titleColor: i === 2 ? C.accent1 : C.text2 });
  s.addText(file, { x: x + 0.15, y: 1.72, w: 2.5, h: 0.3, fontSize: 11, color: "86868B", fontFace: "Courier New",
    margin: 0, isTextBox: true, objectName: `contract${i}-file` });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: x + 0.15, y: 2.08, w: 2.5, h: 0.95, rectRadius: 0.06,
    fill: { color: C.text2 }, line: { color: C.text2, width: 0 }, objectName: `contract${i}-codebg` });
  s.addText(code, { x: x + 0.25, y: 2.13, w: 2.3, h: 0.85, fontSize: 11, color: C.background1,
    fontFace: "Courier New", valign: "middle", margin: 0, isTextBox: true, objectName: `contract${i}-code` });
  s.addText(desc, { x: x + 0.15, y: 3.15, w: 2.5, h: 1.45, fontSize: 12, color: C.text1, valign: "top", margin: 0,
    isTextBox: true, objectName: `contract${i}-desc` });
  if (i < 2) s.addText("›", { x: x + 2.8, y: 2.6, w: 0.3, h: 0.5, fontSize: 28, bold: true, color: C.accent1,
    align: "center", valign: "middle", margin: 0, isTextBox: true, objectName: `contract${i}-arrow` });
});
s.addNotes("La clave para trabajar en paralelo fueron los contratos. Primero escribí los headers con las funciones de cada módulo. Después, una versión débil de cada función para que el kernel compile aunque el módulo no exista. Cada agente escribe la versión real en su carpeta, y el enlazador elige siempre la fuerte.");

// ---------------------------------------------------------------- 8. memory map
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Arquitectura" });
s.addText("Mapa de memoria virtual del kernel", { placeholder: "title" });
const hdr = (t) => ({ text: t, options: { bold: true, color: C.background1, fill: { color: C.text2 } } });
const rows = [
  [hdr("Dirección"), hdr("Uso"), hdr("Dueño")],
  ["0xffff8000_00000000", "Toda la RAM física mapeada por Limine (HHDM)", "Limine / mm"],
  ["0xffffc000_00000000", "Heap: arena para reservas pequeñas (64 GiB)", "mm"],
  ["+ 64 GiB", "Heap: reservas ≥ 256 KiB con página de guarda", "mm"],
  ["0xffffd000_00000000", "Registros de dispositivos PCI, sin caché", "pci"],
  ["0xffffffff_80000000", "Imagen del kernel: código y datos", "linker"],
  ["0x0000_0000_0040_0000", "Programas de usuario en ring 3 (uno por proceso)", "proc"],
];
s.addTable(rows, { x: 0.5, y: 1.2, w: 9.0, colW: [2.6, 4.9, 1.5], fontSize: 12, color: C.text1,
  border: { type: "solid", color: "D6D6DC", pt: 0.75 }, rowH: 0.42, valign: "middle", margin: 0.06,
  autoPage: false, objectName: "memmap" });
s.addText("Cada zona vive en una entrada distinta de la tabla de páginas de nivel 4, así que nunca se pisan.",
  { x: 0.5, y: 4.4, w: 9, h: 0.4, fontSize: 13, italic: true, color: C.text2, margin: 0, isTextBox: true,
    objectName: "memmap-note" });
s.addNotes("El kernel vive en la mitad alta del espacio de direcciones. Limine mapea toda la RAM en el HHDM. El heap tiene su propia zona, igual que los registros de dispositivos PCI. Los programas de usuario viven en la mitad baja, cada uno con su propio espacio.");

// ---------------------------------------------------------------- 9. section
pres.addSection({ title: "Pasos" });
s = pres.addSlide({ masterName: "DARK_SECTION", sectionTitle: "Pasos" });
s.addText("02", { placeholder: "kicker" });
s.addText("Pasos de construcción", { placeholder: "title" });
s.addText("Fases, rondas de agentes e integración continua", { placeholder: "body" });
s.addNotes("Segunda parte: en qué orden se construyó y cómo se coordinó el trabajo.");

// ---------------------------------------------------------------- 10. phases grid
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Pasos" });
s.addText("16 fases: 12 hechas, 1 en curso, 3 pendientes", { placeholder: "title" });
const phases = [
  ["0", "Setup y tests", 2], ["1", "Arranque Limine", 2], ["2", "CPU e interrupciones", 2], ["3", "Memoria", 2],
  ["4", "Teclado, ratón, reloj", 2], ["5", "Gráficos", 2], ["6", "Estilo macOS", 2], ["7", "Ventanas y dock", 2],
  ["8", "Multitarea", 2], ["9", "Ring 3 y syscalls", 2], ["10", "Disco y FAT32", 2], ["11", "Apps (Reloj listo)", 1],
  ["12", "Red TCP/IP", 2], ["13", "HTTPS y navegador", 0], ["14", "Pulido", 0], ["15", "Port a ARM", 0],
];
const stCol = [HEX.lt2, HEX.accent5, HEX.accent4];
const stText = [C.text1, C.text1, C.background1];
phases.forEach(([n, name, st], i) => {
  const col = i % 4, row = Math.floor(i / 4);
  const x = 0.5 + col * 2.28, y = 1.15 + row * 0.86;
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 2.1, h: 0.72, rectRadius: 0.08,
    fill: { color: stCol[st] }, line: { color: st === 0 ? "D6D6DC" : stCol[st], width: 0.75 },
    objectName: `phase${n}` });
  s.addText([{ text: n + "  ", options: { bold: true, fontSize: 18 } }, { text: name, options: { fontSize: 13 } }],
    { x: x + 0.12, y, w: 1.9, h: 0.72, color: stText[st], valign: "middle", margin: 0, isTextBox: true,
      objectName: `phase${n}-t` });
});
[["Hecha", 2], ["En curso", 1], ["Pendiente", 0]].forEach(([t, st], i) => {
  const x = 0.5 + i * 1.5;
  s.addShape(pres.shapes.OVAL, { x, y: 4.68, w: 0.18, h: 0.18, fill: { color: stCol[st] },
    line: { color: "B0B0B8", width: 0.5 }, objectName: `legend${i}-dot` });
  s.addText(t, { x: x + 0.25, y: 4.62, w: 1.2, h: 0.3, fontSize: 11, color: C.text1, margin: 0, isTextBox: true,
    objectName: `legend${i}` });
});
s.addNotes("El plan tiene 16 fases. Están hechas de la 0 a la 10 y la 12, la red. La 11, apps, está en curso: el Reloj ya funciona. Quedan HTTPS con navegador, pulido y el port a ARM.");

// ---------------------------------------------------------------- 11. agents
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Pasos" });
s.addText("Dos rondas, siete agentes, cero conflictos", { placeholder: "title" });
const r1 = [
  ["arch", "kernel/arch/x86_64", "GDT, IDT, excepciones, PIC, timer"],
  ["mm", "kernel/mm", "Memoria física, paginación, heap"],
  ["drivers", "kernel/drivers", "PS/2 teclado y ratón, RTC"],
  ["ui", "kernel/gfx · wm · apps", "Escritorio estilo macOS"],
];
const r2 = [
  ["proc", "kernel/proc", "Hilos, scheduler, ring 3"],
  ["fs", "kernel/fs", "VFS, AHCI, FAT32"],
  ["net", "kernel/net", "e1000, TCP/IP, DHCP, DNS, HTTP"],
];
s.addText("Ronda 1", { x: 0.5, y: 1.05, w: 2, h: 0.3, fontSize: 13, bold: true, color: C.accent1, margin: 0,
  isTextBox: true, objectName: "r1-label" });
r1.forEach(([n, f, d], i) => {
  const x = 0.5 + i * 2.3;
  windowCard(s, x, 1.38, 2.1, 1.5, n, `r1a${i}`);
  s.addText(f, { x: x + 0.15, y: 2.05, w: 1.85, h: 0.28, fontSize: 10, color: "86868B", fontFace: "Courier New",
    margin: 0, isTextBox: true, objectName: `r1a${i}-f` });
  s.addText(d, { x: x + 0.15, y: 2.32, w: 1.85, h: 0.52, fontSize: 12, color: C.text1, valign: "top", margin: 0,
    isTextBox: true, objectName: `r1a${i}-d` });
});
s.addText("Ronda 2 (antes: el integrador añade PCI y puntos de entrada)", { x: 0.5, y: 3.0, w: 7, h: 0.3,
  fontSize: 13, bold: true, color: C.accent1, margin: 0, isTextBox: true, objectName: "r2-label" });
r2.forEach(([n, f, d], i) => {
  const x = 0.5 + i * 2.3;
  windowCard(s, x, 3.33, 2.1, 1.5, n, `r2a${i}`);
  s.addText(f, { x: x + 0.15, y: 4.0, w: 1.85, h: 0.28, fontSize: 10, color: "86868B", fontFace: "Courier New",
    margin: 0, isTextBox: true, objectName: `r2a${i}-f` });
  s.addText(d, { x: x + 0.15, y: 4.27, w: 1.85, h: 0.52, fontSize: 12, color: C.text1, valign: "top", margin: 0,
    isTextBox: true, objectName: `r2a${i}-d` });
});
s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 7.4, y: 3.33, w: 2.1, h: 1.5, rectRadius: 0.08,
  fill: { color: C.background2 }, line: { color: C.background2, width: 0 }, objectName: "rule-card" });
s.addText("Cada agente: su worktree de git, su carpeta y make test en verde antes de entregar.",
  { x: 7.55, y: 3.43, w: 1.8, h: 1.3, fontSize: 12, color: C.text2, valign: "middle", margin: 0, isTextBox: true,
    objectName: "rule-text" });
s.addNotes("Hubo dos rondas. En la primera, cuatro agentes: CPU, memoria, drivers y escritorio. Antes de la segunda añadí el bus PCI, que comparten disco y red, y los puntos de entrada en kmain. En la segunda, tres agentes: multitarea, archivos y red. Cada uno trabajó en su propio worktree y en su carpeta, así que nunca se pisaron.");

// ---------------------------------------------------------------- 12. integration loop
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Pasos" });
s.addText("El ciclo de integración", { placeholder: "title" });
const loop = [
  ["Contrato", "Header + reserva débil + hueco en kmain"],
  ["Agente", "Trabaja en su worktree y su carpeta"],
  ["make test", "BIOS y UEFI en verde, con [OK]"],
  ["Merge", "El integrador fusiona la rama en main"],
  ["Build limpio", "make clean + los 23 tests juntos"],
  ["Push", "Sube a GitHub y actualiza docs"],
];
loop.forEach(([t, d], i) => {
  const x = 0.5 + i * 1.53;
  s.addShape(pres.shapes.OVAL, { x: x + 0.38, y: 1.35, w: 0.6, h: 0.6, fill: { color: i === 2 ? C.accent4 : C.accent1 },
    line: { color: C.background1, width: 0 }, objectName: `loop${i}-c` });
  s.addText(String(i + 1), { x: x + 0.38, y: 1.35, w: 0.6, h: 0.6, fontSize: 20, bold: true, color: C.background1,
    align: "center", valign: "middle", margin: 0, isTextBox: true, objectName: `loop${i}-n` });
  if (i < loop.length - 1) s.addShape(pres.shapes.LINE, { x: x + 1.05, y: 1.65, w: 0.82, h: 0,
    line: { color: "C9C3F5", width: 2, endArrowType: "triangle" }, objectName: `loop${i}-arrow` });
  s.addText(t, { x, y: 2.1, w: 1.36, h: 0.4, fontSize: 15, bold: true, color: C.text2, align: "center", margin: 0,
    isTextBox: true, objectName: `loop${i}-t` });
  s.addText(d, { x, y: 2.5, w: 1.36, h: 0.85, fontSize: 12, color: C.text1, align: "center", valign: "top", margin: 0,
    isTextBox: true, objectName: `loop${i}-d` });
});
windowCard(s, 0.5, 3.6, 9.0, 1.3, "Incidencias resueltas por el camino", "incidents");
s.addText("Caída de la API: dos agentes se reanudaron sin perder trabajo  ·  El heap no aguantaba hilos: se le añadió un lock  ·  SeaBIOS arrancaba desde el disco: -boot order=d",
  { x: 0.65, y: 4.32, w: 8.7, h: 0.5, fontSize: 12, color: C.text1, valign: "top", margin: 0, isTextBox: true,
    objectName: "incidents-text" });
s.addNotes("Cada módulo pasó por el mismo ciclo: contrato, agente, test, merge, build limpio con todos los tests y push. Por el camino hubo incidencias: una caída de la API, el heap sin protección para varios hilos y SeaBIOS intentando arrancar desde el disco FAT32. Todas se resolvieron dentro del ciclo.");

// ---------------------------------------------------------------- 13. verification
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Pasos" });
s.addText("Cómo se verifica sin mirar la pantalla", { placeholder: "title" });
s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y: 1.15, w: 5.0, h: 3.75, rectRadius: 0.08,
  fill: { color: C.text2 }, line: { color: C.text2, width: 0 }, shadow: shadow(), objectName: "log-bg" });
lights(s, 0.65, 1.29, "log");
const logLines = [
  "[OK] arch gdt/idt/pic/pit",
  "[OK] mm total=237 MiB free=237 MiB",
  "[OK] sched preempt: sleep(20)=20ms",
  "hello from ring 3",
  "[OK] input ps2 kbd=yes mouse=yes",
  "[OK] fat32 selftest: write, verify",
  "[OK] dhcp ip=10.0.2.15",
  "[OK] http GET example.com -> 200 OK",
  "[OK] desktop 1280x800, 2 window(s)",
  "[TEST] boot OK",
];
s.addText(logLines.map((t, i) => ({ text: t, options: { breakLine: i < logLines.length - 1,
  color: t.startsWith("[TEST]") ? C.accent4 : (t.startsWith("[OK]") ? "C9C3F5" : C.accent2) } })),
  { x: 0.7, y: 1.55, w: 4.65, h: 3.2, fontSize: 11, fontFace: "Courier New", valign: "top", margin: 0,
    isTextBox: true, objectName: "log-text" });
const ver = [
  ["Modo test", "Una ISO aparte arranca con cmdline test y apaga QEMU con código 33 vía isa-debug-exit."],
  ["BIOS y UEFI", "Cada test se repite con SeaBIOS y con OVMF."],
  ["Capturas por QMP", "QEMU guarda el escritorio en PNG y Claude revisa la imagen."],
];
ver.forEach(([t, d], i) => {
  const y = 1.2 + i * 1.25;
  s.addShape(pres.shapes.OVAL, { x: 5.85, y: y + 0.05, w: 0.42, h: 0.42, fill: { color: C.accent1 },
    line: { color: C.accent1, width: 0 }, objectName: `ver${i}-c` });
  s.addText("✓", { x: 5.85, y: y + 0.05, w: 0.42, h: 0.42, fontSize: 16, bold: true, color: C.background1,
    align: "center", valign: "middle", margin: 0, isTextBox: true, objectName: `ver${i}-check` });
  s.addText(t, { x: 6.4, y, w: 3.1, h: 0.4, fontSize: 15, bold: true, color: C.text2, margin: 0, isTextBox: true,
    objectName: `ver${i}-t` });
  s.addText(d, { x: 6.4, y: y + 0.4, w: 3.1, h: 0.75, fontSize: 12, color: C.text1, valign: "top", margin: 0,
    isTextBox: true, objectName: `ver${i}-d` });
});
s.addNotes("La verificación es automática. Cada módulo escribe su línea OK por el puerto serie. En modo test el kernel apaga QEMU con un código de salida concreto, y el script solo da PASS si ese código y la línea final aparecen. Se repite con BIOS y con UEFI, y las capturas se toman por QMP para que Claude revise el diseño.");

// ---------------------------------------------------------------- 14. result
s = pres.addSlide({ masterName: "CONTENT", sectionTitle: "Pasos" });
s.addText("El resultado: un escritorio estilo macOS", { placeholder: "title" });
s.addImage({ path: IMG + "clock-app.png", x: 0.5, y: 1.15, w: 5.6, h: 3.5, shadow: shadow(), objectName: "result-img",
  altText: "Escritorio de myOS con las apps Welcome, Notes y Clock" });
body(s, 6.4, 1.2, 3.1, 3.5, [
  "Barra de menú y dock esmerilados",
  "Botones de semáforo y sombras suaves",
  "Dock con efecto lupa",
  "Fuente Inter suavizada (OFL)",
  "Wallpaper generado por código",
  "Reloj analógico con hora local",
  "Sin recursos de Apple",
], "result-list", 14);
s.addNotes("Este es el resultado: un escritorio con aspecto de macOS, pero sin ningún recurso de Apple. El vidrio esmerilado se calcula una sola vez al arrancar para no gastar CPU, y solo se redibujan las zonas que cambian. La app Reloj es la primera de la fase 11.");

// ---------------------------------------------------------------- 15. next
pres.addSection({ title: "Cierre" });
s = pres.addSlide({ masterName: "DARK_SECTION", sectionTitle: "Cierre" });
s.addText("SIGUIENTE", { placeholder: "kicker" });
s.addText("Apps, HTTPS y pulido", { placeholder: "title" });
s.addText("Terminal, Archivos y Ajustes · TLS y navegador · arranque en 1,5 s · port a ARM para el M3", { placeholder: "body" });
[["v0.1 escritorio", 100], ["Plan completo", 75]].forEach(([t, p], i) => {
  const y = 4.25 + i * 0.5;
  s.addText(t, { x: 0.8, y, w: 2.0, h: 0.32, fontSize: 13, color: C.background2, margin: 0, isTextBox: true,
    valign: "middle", objectName: `prog${i}-t` });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 2.8, y: y + 0.06, w: 5.4, h: 0.2, rectRadius: 0.1,
    fill: { color: C.background1, transparency: 80 }, line: { color: C.background1, width: 0, transparency: 100 },
    objectName: `prog${i}-track` });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 2.8, y: y + 0.06, w: 5.4 * p / 100, h: 0.2, rectRadius: 0.1,
    fill: { color: i === 0 ? C.accent4 : C.accent2 }, line: { color: C.background1, width: 0, transparency: 100 },
    objectName: `prog${i}-bar` });
  s.addText(p + " %", { x: 8.3, y, w: 0.9, h: 0.32, fontSize: 13, bold: true, color: C.background1, margin: 0,
    isTextBox: true, valign: "middle", objectName: `prog${i}-p` });
});
s.addNotes("La versión 0.1 está completa y el plan va al 75 por ciento. Lo siguiente es terminar las apps, añadir HTTPS con un navegador, pulir el arranque y, opcionalmente, portar myOS a ARM para que corra nativo en el Mac.");

(async () => {
  await pres.writeFile({ fileName: OUT });
  await applyTheme(OUT, THEME);
  console.log("wrote", OUT);
})();
