/*! Feel Figure 03 adapted to one resting point for FullWinTB. MIT © diip3sh. */
(() => {
  "use strict";

  const chinese = {
    Settings: "设置", Motion: "回弹手感", General: "常规", Language: "语言",
    "Third-party licenses": "第三方许可", Close: "关闭",
    "Return style": "回弹方式", Elastic: "弹性", Simple: "简洁",
    "A soft bounce as the icons settle.": "回到原位时略微回摆。",
    "The icons come to rest at their original position.": "图标回到原位即停。",
    "Feel preview": "手感预览", "Preview card": "预览卡片", "Try return": "试回弹",
    "Drag the card or scroll here to preview.": "拖动卡片或滚动鼠标试手感。",
    "Wheel step": "每格位移", "Hold time": "停留时间",
    Taskbar: "任务栏", "Wheel direction": "滚轮方向", "Drag direction follows your pointer.": "拖拽方向由鼠标移动决定。",
    Horizontal: "左右移动", Vertical: "上下移动", "Taskbar text": "任务栏文字",
    "Automatic contrast": "自动黑白配色", "Follow system": "跟随系统", "White text": "白字", "Black text": "黑字",
    Controls: "操作", "Toggle shortcut": "切换快捷键", Change: "更改",
    "Icon positions": "图标位置", "Reset positions": "归位", Cancel: "取消", Save: "保存",
    Presets: "手感预设", "Describe the feel": "输入手感关键词",
    "Combine presets, e.g. light and snappy": "可组合预设，如「轻盈、利落」",
    "Unknown preset": "未识别的关键词", "Fine tuning": "物理参数",
    Mass: "质量", Damping: "阻尼", "Near pull": "近处吸合", "Far pull": "远处回拉",
    Frequency: "频率", "Damping ratio": "阻尼比", Stiffness: "刚度",
    "Motion data and export": "运动数据与导出", Path: "轨迹", Position: "位移", Speed: "速度",
    "Force field": "力场", Measured: "实测", Predicted: "预测", Peak: "峰值",
    "Pulls toward the origin": "回拉至原位", "Pulls toward each stop": "拉向各个停靠点",
    Held: "拖动中", Released: "松手", Moving: "移动中", Settled: "已归位",
    Measurements: "测量值", Attractors: "吸合点数", "Capture radius": "吸合半径",
    "Coast at 1000 px/s": "初速 1000 px/s 的滑行距离", "Resting point": "停靠位置", Origin: "原位",
    "Escape speed": "脱离速度", Overshoot: "过冲", "Settle time": "稳定时间",
    Export: "导出", "Export to": "导出格式", "Copy code": "复制代码", Copied: "已复制", "Copy failed": "复制失败",
    "Preview shortcuts": "预览快捷键", "Top left": "左上", "Top right": "右上",
    "Bottom right": "右下", "Bottom left": "左下", "Fling across": "抛出卡片",
    Playback: "播放方式", "JavaScript loop": "逐帧模拟", "CSS keyframes": "CSS 关键帧",
    "Simulates each frame.": "每帧计算卡片的位置。", keyframes: "个关键帧",
    "Precomputes the animation on release.": "松手时计算轨迹，再由浏览器播放。",
    heavy: "沉稳", snappy: "利落", bouncy: "弹跳", soft: "柔和", light: "轻盈",
    stiff: "紧实", wobbly: "摇摆", precise: "精准",
    crisp: "干脆", quick: "迅速", instant: "瞬时", firm: "稳固", tight: "紧致",
    smooth: "顺滑", gentle: "轻柔", calm: "平缓", lazy: "慵懒", playful: "活泼",
    springy: "富有弹性", loose: "松散", elastic: "有弹性", weighty: "厚重", solid: "扎实",
    massive: "沉重", airy: "飘逸", feathery: "羽量", magnetic: "磁吸", sticky: "黏滞",
    resistant: "滞涩", clicky: "吸扣", tactile: "有触感",
  };
  const presetKeys = new Map(Object.entries(chinese).filter(([key]) => /^[a-z]+$/.test(key)).map(([key, value]) => [value, key]));
  function parseWords(input) {
    const unknown = [];
    const words = input.replace(/[\u3400-\u9fff]+/g, phrase => {
      if (presetKeys.has(phrase)) return ` ${presetKeys.get(phrase)} `;
      unknown.push(phrase);
      return " ";
    });
    const parsed = Feel.words(words);
    parsed.unknown.push(...unknown);
    return parsed;
  }

  const byId = id => document.getElementById(id);
  const host = byId("rig-card");
  const screen = byId("screen");
  const card = byId("pip");
  const timing = document.querySelector(".au-timing");

  const [mass, damping, click, reach] = initialMotion.slice(0, 4).map(value => value / 100);
  const wheelStep = byId("wheel-step");
  const returnHold = byId("return-hold");
  const wheelDirection = byId("wheel-direction");
  const taskbarInk = byId("taskbar-ink");
  const language = byId("language");
  const returnStyle = () => Number(document.querySelector("[name=return-style]:checked").value);
  const t = key => language.value === "0" ? chinese[key] ?? key : key;
  [wheelStep.value, returnHold.value, wheelDirection.value, taskbarInk.value, language.value] =
    [...initialMotion.slice(4, 8), initialMotion[9]];
  document.querySelector(`[name=return-style][value="${initialMotion[8]}"]`).checked = true;

  // The native groups return independently on each axis. Match that here
  // using the existing Feel force and solver for the preview and its traces.
  const magnet = Feel.points({ stops: [{ x: 0, y: 0 }], reachFreq: reach, clickFreq: click, clickRadius: 28 });
  const home = magnet.stops[0];
  const field = Object.assign((x, y, vx, vy, body) =>
    [magnet(x, home.y, vx, 0, body)[0], magnet(home.x, y, 0, vy, body)[1]], magnet);
  const travel = { x: [0, 1], y: [0, 1] };
  const dockTop = verticalDirection < 0;

  class PreviewBody extends Feel.Body2D {
    ghost(state = this.state()) {
      const ghost = new PreviewBody({
        ...this.params, ...state, forces: this.forces, target: this.target, min: this.min, max: this.max,
        wallFreq: this.wallFreq, restV: this.restV, restX: this.restX,
      });
      ghost.returnStyle = this.returnStyle;
      return ghost;
    }

    step(dt) {
      if (this.hand) {
        super.step(dt);
        this.limitAxis("x", "vx", this.x, true);
        this.limitAxis("y", "vy", this.y, true);
        return;
      }
      const steps = Math.max(1, Math.ceil(dt * 240));
      for (let i = 0; i < steps; i++) {
        const beforeX = this.x, beforeY = this.y;
        super.step(dt / steps);
        this.limitAxis("x", "vx", beforeX, false);
        this.limitAxis("y", "vy", beforeY, false);
      }
    }

    limitAxis(axis, velocity, before, held) {
      const origin = home[axis];
      const first = this.min[axis], last = this.max[axis];
      const after = this[axis];
      if (!held) {
        if (this.returnStyle === 1 && before !== origin && (before - origin) * (after - origin) <= 0) {
          this[axis] = origin;
          this[velocity] = 0;
          return;
        }
        if (this.returnStyle === 0 && first < last &&
            ((first === origin && after < first) || (last === origin && after > last))) {
          this[axis] = 2 * origin - after;
          this[velocity] = -this[velocity];
        }
      }
      const bounded = Math.max(first, Math.min(last, this[axis]));
      if (bounded !== this[axis]) {
        this[axis] = bounded;
        this[velocity] = -0.24 * this[velocity];
      }
    }
  }

  const body = new PreviewBody({
    ...Feel.words("snappy").params, mass, damping, target: null, forces: [field],
    min: { x: 0, y: 0 }, max: { x: 1, y: 1 }, name: "card",
    onFrame: moving => Feel.paint(styles(moving)),
  });
  body.returnStyle = returnStyle();

  function styles(moving) {
    return [[card, { transform: `translate(${moving.x.toFixed(2)}px, ${moving.y.toFixed(2)}px)` }]];
  }

  function resizeStage() {
    if (!screen.clientWidth) return;
    travel.x[1] = Math.max(0, screen.clientWidth - card.offsetWidth - 16);
    travel.y[1] = Math.max(0, screen.clientHeight - card.offsetHeight - 16);
    body.max.x = travel.x[1];
    body.max.y = travel.y[1];
    home.x = travel.x[1] * 0.66;
    home.y = dockTop ? 0 : travel.y[1];
    body.jump(home);
  }

  const tryReturn = () => {
    body.jump(wheelDirection.value === "0"
      ? { x: Math.max(0, home.x - Math.min(70, home.x)), y: home.y }
      : { x: home.x, y: home.y + verticalDirection * -Math.min(64, travel.y[1]) });
    body.wake();
  };
  const wheelImpulse = () => Math.min(8000, wheelStep.valueAsNumber * body.damping / body.mass);
  const fling = () => body.kick(-wheelImpulse(), (dockTop ? 1 : -1) * wheelImpulse() * 0.7);
  const towardCorner = (right, bottom) => {
    body.sample();
    const dx = (right ? travel.x[1] : 0) - body.x;
    const dy = (bottom ? travel.y[1] : 0) - body.y;
    const rate = Math.min(body.damping / body.mass, 8000 / Math.max(1, Math.hypot(dx, dy)));
    body.kick(dx * rate, dy * rate);
  };

  Feel.drag(card, body, { axis: "xy", onTap: tryReturn });
  card.addEventListener("click", event => { if (event.detail === 0) tryReturn(); });
  card.addEventListener("keydown", event => {
    const direction = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[event.key];
    if (!direction) return;
    body.kick(direction[0] * wheelImpulse(), direction[1] * wheelImpulse());
    event.preventDefault();
  });

  const rounded = (value, minimum, maximum) => Math.round(Math.max(minimum, Math.min(maximum, value)) * 100);
  const profile = () => [
    rounded(body.mass, 0.15, 8), rounded(body.damping, 2, 160),
    rounded(field.cfg.clickFreq, 1, 14), rounded(field.cfg.reachFreq, 0.2, 3),
    wheelStep.valueAsNumber, returnHold.valueAsNumber, Number(wheelDirection.value),
    Number(taskbarInk.value), returnStyle(), Number(language.value),
  ];
  const send = (command, values = []) => window.chrome?.webview?.postMessage([command, ...values].join(" "));
  const tune = () => send("tune", profile());

  function showTiming() {
    wheelStep.nextElementSibling.value = `${wheelStep.value} px`;
    const seconds = returnHold.valueAsNumber / 1000;
    returnHold.nextElementSibling.value = `${Number.isInteger(seconds) ? seconds.toFixed(1) : seconds} ${language.value === "0" ? "秒" : "s"}`;
    for (const control of [wheelStep, returnHold])
      control.setAttribute("aria-valuetext", control.nextElementSibling.value);
  }

  const matchingWord = click === 5 && reach === 0.85 &&
    ["heavy", "snappy", "bouncy", "soft", "light", "stiff", "wobbly", "precise"].find(word => {
      const preset = Feel.words(word).params;
      return rounded(preset.mass, 0.15, 8) === initialMotion[0] && rounded(preset.damping, 2, 160) === initialMotion[1];
    });
  let handoff = null;
  const rig = Feel.rig(host, {
    body, stage: screen, travel, tryReturn, parseWords, translate: t,
    feel: matchingWord || "",
    handoff(on) {
      if (on) handoff = Feel.handoff(body, styles, { read: () => {
        const transform = new DOMMatrixReadOnly(getComputedStyle(card).transform);
        return { x: transform.m41, y: transform.m42 };
      }});
      else if (handoff) { handoff.off(); handoff = null; }
    },
    actions: [
      { key: "1", label: "Top left", run: () => towardCorner(false, false) },
      { key: "2", label: "Top right", run: () => towardCorner(true, false) },
      { key: "3", label: "Bottom right", run: () => towardCorner(true, true) },
      { key: "4", label: "Bottom left", run: () => towardCorner(false, true) },
      { key: "F", label: "Fling across", run: fling },
    ],
    onParams() {
      [body.mass, body.damping, field.cfg.clickFreq, field.cfg.reachFreq] = profile().slice(0, 4).map(value => value / 100);
      tune();
    },
  });

  host.insertBefore(timing, host.querySelector(".fl-details"));
  resizeStage();
  new ResizeObserver(resizeStage).observe(screen);
  host.querySelector(".fl-bench").addEventListener("wheel", event => {
    const units = event.deltaMode === 1 ? event.deltaY / 3 : event.deltaMode === 2 ? Math.sign(event.deltaY) : event.deltaY / 100;
    if (!units || event.ctrlKey) return;
    const push = Math.max(-3, Math.min(3, units)) * wheelImpulse();
    if (wheelDirection.value === "0") body.kick(push, 0);
    else body.kick(0, push);
    event.preventDefault();
  }, { passive: false });

  function showReturnStyle() {
    byId("return-description").textContent = t(returnStyle() === 0
      ? "A soft bounce as the icons settle." : "The icons come to rest at their original position.");
  }

  function localize() {
    document.documentElement.lang = language.value === "0" ? "zh-CN" : "en";
    document.title = `FullWinTB · ${t("Settings")}`;
    for (const node of document.querySelectorAll("[data-text]")) node.textContent = t(node.dataset.text);
    for (const node of document.querySelectorAll("[data-aria]")) node.setAttribute("aria-label", t(node.dataset.aria));
    for (const node of document.querySelectorAll("[data-placeholder]")) node.placeholder = t(node.dataset.placeholder);
    showTiming();
    showReturnStyle();
    rig.localize();
  }

  const tabs = [byId("motion-tab"), byId("general-tab")];
  const activate = active => {
    for (const tab of tabs) {
      const selected = tab === active;
      tab.setAttribute("aria-selected", String(selected));
      tab.tabIndex = selected ? 0 : -1;
      byId(tab.getAttribute("aria-controls")).hidden = !selected;
    }
    document.querySelector("main").scrollTop = 0;
  };
  for (const tab of tabs) {
    tab.addEventListener("click", () => activate(tab));
    tab.addEventListener("keydown", event => {
      const next = event.key === "Home" ? tabs[0] : event.key === "End" ? tabs[1] :
        ["ArrowLeft", "ArrowRight"].includes(event.key) ? tabs.find(other => other !== tab) : null;
      if (!next) return;
      event.preventDefault();
      activate(next);
      next.focus();
    });
  }

  for (const control of [wheelStep, returnHold]) control.addEventListener("input", () => { showTiming(); tune(); });
  for (const control of [wheelDirection, taskbarInk]) control.addEventListener("change", tune);
  for (const radio of document.querySelectorAll("[name=return-style]")) radio.addEventListener("change", () => {
    body.returnStyle = returnStyle();
    showReturnStyle();
    tryReturn();
    tune();
  });
  language.addEventListener("change", () => { localize(); tune(); });
  byId("shortcut").addEventListener("click", () => send("hotkey"));
  byId("reset-icons").addEventListener("click", () => send("reset"));
  window.chrome?.webview?.addEventListener("message", event => {
    if (typeof event.data === "string") byId("shortcut-key").textContent = event.data;
  });
  byId("cancel").addEventListener("click", () => send("cancel"));
  const notices = byId("notices");
  byId("open-notices").addEventListener("click", () => notices.showModal());
  const save = () => {
    const words = host.querySelector(".ff-input input");
    if (document.activeElement === words) words.blur();
    send("save", profile());
  };
  byId("save").addEventListener("click", save);
  window.addEventListener("keydown", event => {
    if (notices.open) return;
    if (event.key === "Escape") send("cancel");
    if (event.key === "Enter" && event.ctrlKey) { event.preventDefault(); save(); }
  }, true);
  localize();
  send("ready");
})();
