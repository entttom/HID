(() => {
  "use strict";

  const NAVBAR_ITEM_ID = "hid-automation-navbar";
  const MODAL_ID = "hid-automation-modal";
  const STYLE_ID = "hid-automation-styles";
  const BACKEND = "/hid-automation/api";

  let pollTimer = null;
  let draftEvents = [];
  let lastAbsPos = null;

  function rootPrefix() {
    const path = window.location.pathname;
    const match = path.match(/^(.*\/)(?:kvm\/?)(?:index\.html)?$/);
    return match ? match[1] : "/";
  }

  // ---- backend / PiKVM API helpers -----------------------------------

  async function api(path, body) {
    const r = await fetch(BACKEND + path, {
      method: body ? "POST" : "GET",
      headers: {"Content-Type": "application/json"},
      body: body ? JSON.stringify(body) : undefined,
    });
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || "Fehler");
    return j.result || j;
  }

  // Talks to PiKVM's own authenticated API on the same origin (not our
  // backend) so the existing, native mouse jiggler is reused as-is instead
  // of being reimplemented.
  async function kvmdGet(path) {
    const r = await fetch(path, {method: "GET"});
    return r.json();
  }

  async function kvmdPost(path) {
    const r = await fetch(path, {method: "POST"});
    return r.json();
  }

  // ---- navbar item -----------------------------------------------------

  function addNavbarItem() {
    if (document.getElementById(NAVBAR_ITEM_ID)) return;

    const navbar = document.getElementById("navbar");
    const macro = document.getElementById("macro-dropdown");
    if (!navbar || !macro) {
      window.setTimeout(addNavbarItem, 250);
      return;
    }

    const root = rootPrefix();
    const item = document.createElement("li");
    item.className = "right";
    item.id = NAVBAR_ITEM_ID;

    const link = document.createElement("a");
    link.className = "menu-item menu-action";
    link.href = `${root}hid-automation/`;
    link.title = "HID Automation";
    link.setAttribute("aria-label", "HID Automation öffnen");

    const icon = document.createElement("img");
    icon.className = "svg-gray";
    icon.src = `${root}share/svg/led-gear.svg`;
    icon.alt = "";

    const label = document.createElement("span");
    label.textContent = " Automation";

    link.append(icon, label);
    item.appendChild(link);

    // A plain click opens the in-page modal. Middle-click / Ctrl|Cmd-click
    // is left alone so the full standalone page still opens in a new tab.
    link.addEventListener("click", (ev) => {
      if (ev.button !== 0 || ev.ctrlKey || ev.metaKey || ev.shiftKey || ev.altKey) return;
      ev.preventDefault();
      toggleModal();
    });

    // PiKVM uses right-floating navbar items. Inserting after Macro places
    // Automation beside the existing Macro/Text controls without touching
    // PiKVM's own HTML or JavaScript files.
    macro.insertAdjacentElement("afterend", item);
  }

  // ---- modal -------------------------------------------------------------

  function injectStyles() {
    if (document.getElementById(STYLE_ID)) return;
    const style = document.createElement("style");
    style.id = STYLE_ID;
    style.textContent = `
      #${MODAL_ID} .modal-window { width: 520px; max-width: 92vw; margin: 20px auto; }
      #${MODAL_ID} .modal-header {
        display: flex; align-items: center; justify-content: space-between;
        text-align: left;
      }
      #${MODAL_ID} .modal-content {
        max-height: 70vh; overflow-y: auto; padding: 12px 14px;
      }
      #${MODAL_ID} button.ha-close {
        background: none; border: none; color: var(--cs-window-closer-default-fg);
        font-size: 15px; height: 22px; width: 22px; padding: 0;
      }
      #${MODAL_ID} button.ha-close:hover { color: var(--cs-control-intensive-fg); }
      #${MODAL_ID} .ha-section {
        margin-bottom: 12px; padding-bottom: 12px;
        border-bottom: var(--border-default-thin);
      }
      #${MODAL_ID} .ha-section:last-child { border-bottom: none; margin-bottom: 0; padding-bottom: 0; }
      #${MODAL_ID} .ha-row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
      #${MODAL_ID} .ha-row + .ha-row { margin-top: 8px; }
      #${MODAL_ID} .ha-row-between { justify-content: space-between; }
      #${MODAL_ID} .ha-muted { color: var(--cs-window-header-default-fg); font-size: 13px; }
      #${MODAL_ID} .ha-err { color: #ff6b6b; font-size: 13px; }
      #${MODAL_ID} .ha-status { font-weight: bold; }
      #${MODAL_ID} input[type=number] {
        width: 56px; background-color: var(--cs-control-default-bg);
        color: var(--cs-control-default-fg); border: var(--border-control-thin);
        border-radius: 4px; height: 26px; padding: 2px 4px; box-sizing: border-box;
      }
      #${MODAL_ID} input[type=time], #${MODAL_ID} input[type=datetime-local] {
        background-color: var(--cs-control-default-bg); color: var(--cs-control-default-fg);
        border: var(--border-control-thin); border-radius: 4px; height: 26px;
        padding: 2px 4px; box-sizing: border-box; font: inherit;
      }
      #${MODAL_ID} label.ha-field { display: flex; flex-direction: column; gap: 3px; font-size: 12px; flex: 1; }
      #${MODAL_ID} label.ha-field input { width: 100%; box-sizing: border-box; }
      #${MODAL_ID} h4 { margin: 0 0 6px 0; font-size: 13px; }
      #${MODAL_ID} button.ha-danger { background-color: #a33; color: #fff; }
      #${MODAL_ID} .ha-events { display: flex; flex-direction: column; gap: 4px; margin-top: 8px; }
      #${MODAL_ID} .ha-event {
        display: flex; justify-content: space-between; align-items: center;
        border-bottom: var(--border-default-thin); padding: 4px 0; font-size: 13px;
      }
      #${MODAL_ID} .ha-event button { height: 22px; padding: 0 8px; font-size: 12px; }
      #${MODAL_ID} .ha-disabled { opacity: 0.5; pointer-events: none; }
      #${MODAL_ID} a { color: var(--cs-thumb-default-bg); }
    `;
    document.head.appendChild(style);
  }

  function contentHtml() {
    return `
      <div class="ha-section">
        <div class="ha-row ha-row-between">
          <span class="ha-status" id="ha-status">Lade…</span>
          <button class="small" id="ha-trigger">Flow jetzt starten</button>
        </div>
        <div class="ha-muted" id="ha-flow"></div>
        <div class="ha-muted" id="ha-next"></div>
        <div class="ha-err" id="ha-error"></div>
      </div>

      <div class="ha-section ha-row ha-row-between" id="ha-jiggler-row">
        <span>Mouse Jiggler</span>
        <div class="switch-box">
          <input type="checkbox" id="ha-jiggler-switch">
          <label for="ha-jiggler-switch"><span class="switch-inner"></span><span class="switch"></span></label>
        </div>
      </div>

      <div class="ha-section">
        <div class="ha-row ha-row-between">
          <span>Automatische Wiederholung</span>
          <div class="switch-box">
            <input type="checkbox" id="ha-auto-switch">
            <label for="ha-auto-switch"><span class="switch-inner"></span><span class="switch"></span></label>
          </div>
        </div>
        <div class="ha-row">
          <label>Min. <input type="number" id="ha-amin" min="1"></label>
          <label>Max. <input type="number" id="ha-amax" min="1"></label>
          <button class="small" id="ha-auto-save">Speichern</button>
        </div>
      </div>

      <div class="ha-section">
        <h4>Klick-Position (fix)</h4>
        <div class="ha-muted" style="margin-bottom:6px">
          Leer = Klick an der aktuellen Mausposition (Standard). Gesetzt = Flow und geplanter
          Ablauf bewegen die Maus vor jedem Klick zuerst an diese absolute Position.
        </div>
        <div class="ha-row">
          <label class="ha-field">X (absolut)<input type="number" id="ha-clickx" placeholder="aktuelle Position"></label>
          <label class="ha-field">Y (absolut)<input type="number" id="ha-clicky" placeholder="aktuelle Position"></label>
        </div>
        <div class="ha-row">
          <button class="small" id="ha-click-capture">Aktuelle Mausposition übernehmen</button>
          <button class="small" id="ha-click-save">Speichern</button>
          <button class="small ha-danger" id="ha-click-clear">Zurücksetzen</button>
        </div>
      </div>

      <div class="ha-section" style="margin-bottom:0; padding-bottom:0">
        <h4>Geplanter Ablauf</h4>
        <div class="ha-muted" id="ha-plan-status" style="margin-bottom:6px"></div>
        <div class="ha-row">
          <label class="ha-field">Start ab<input type="datetime-local" id="ha-start"></label>
        </div>
        <div class="ha-row">
          <label class="ha-field">Bis<input type="time" id="ha-until"></label>
          <label class="ha-field">Events<input type="number" id="ha-count" min="1" max="8"></label>
        </div>
        <div class="ha-row">
          <label class="ha-field">Min. Abstand<input type="number" id="ha-pmin" min="1"></label>
          <label class="ha-field">Max. Abstand<input type="number" id="ha-pmax" min="1"></label>
        </div>
        <div class="ha-row">
          <button class="small" id="ha-calc">Events berechnen</button>
          <button class="small" id="ha-plan-start">Plan starten</button>
          <button class="small ha-danger" id="ha-plan-stop">Stoppen</button>
        </div>
        <div id="ha-events" class="ha-events"></div>
      </div>
    `;
  }

  function fmt(ts) {
    return ts ? new Date(ts * 1000).toLocaleString() : "—";
  }

  function $ha(id) {
    return document.getElementById(id);
  }

  function setDefaultStart() {
    const el = $ha("ha-start");
    if (el && !el.value) {
      const d = new Date();
      d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
      el.value = d.toISOString().slice(0, 16);
    }
  }

  function renderEvents(events) {
    const el = $ha("ha-events");
    if (!el) return;
    el.innerHTML = "";
    events.forEach((ts) => {
      const div = document.createElement("div");
      div.className = "ha-event";
      div.innerHTML = `<span>${fmt(ts)}</span><button class="small ha-danger">Löschen</button>`;
      div.querySelector("button").addEventListener("click", async () => {
        draftEvents = (draftEvents.length ? draftEvents : events).filter((x) => x !== ts);
        await api("/schedule/replace", {times: draftEvents}).catch(showModalErr);
        renderEvents(draftEvents);
      });
      el.appendChild(div);
    });
  }

  function showModalErr(e) {
    const el = $ha("ha-error");
    if (el) el.textContent = e.message;
  }

  async function calculatePlan() {
    try {
      const result = await api("/planner/calculate", {
        startAt: $ha("ha-start").value,
        until: $ha("ha-until").value,
        events: +$ha("ha-count").value,
        min: +$ha("ha-pmin").value,
        max: +$ha("ha-pmax").value,
      });
      draftEvents = result.events;
      await api("/schedule/replace", {times: draftEvents});
      renderEvents(draftEvents);
      await tick();
    } catch (e) {
      showModalErr(e);
    }
  }

  async function startPlan() {
    try {
      await api("/schedule/start", {when: $ha("ha-start").value});
      draftEvents = [];
      await tick();
    } catch (e) {
      showModalErr(e);
    }
  }

  async function stopPlan() {
    try {
      await api("/schedule/stop", {});
      draftEvents = [];
      await tick();
    } catch (e) {
      showModalErr(e);
    }
  }

  async function tick() {
    try {
      const state = await api("/status");
      $ha("ha-status").textContent = state.flowRunning ? "Flow läuft" : "Bereit";
      $ha("ha-flow").textContent = `Zustand: ${state.flowState} · Letzte Auslösung: ${state.lastTrigger || "—"}`;
      $ha("ha-next").textContent = `Nächste Auslösung: ${fmt(state.nextTriggerAt)}`;
      $ha("ha-error").textContent = state.lastError || "";
      $ha("ha-auto-switch").checked = !!state.auto;
      $ha("ha-amin").value = state.autoMinMinutes;
      $ha("ha-amax").value = state.autoMaxMinutes;
      if (document.activeElement !== $ha("ha-clickx")) {
        $ha("ha-clickx").value = state.clickX ?? "";
      }
      if (document.activeElement !== $ha("ha-clicky")) {
        $ha("ha-clicky").value = state.clickY ?? "";
      }
      $ha("ha-until").value = state.plannerUntil;
      $ha("ha-count").value = state.plannerEvents;
      $ha("ha-pmin").value = state.plannerMinMinutes;
      $ha("ha-pmax").value = state.plannerMaxMinutes;
      const planStatus = $ha("ha-plan-status");
      if (planStatus) {
        planStatus.textContent = state.manualPlanActive
          ? `● Plan aktiv — Start: ${fmt(state.manualStartAt)}`
          : "○ Plan inaktiv";
        planStatus.style.color = state.manualPlanActive ? "#74d67a" : "";
      }
      if (!draftEvents.length) renderEvents(state.schedules || []);
    } catch (e) {
      if ($ha("ha-error")) $ha("ha-error").textContent = e.message;
    }

    try {
      const hid = await kvmdGet("/api/hid");
      if (hid.ok && hid.result && hid.result.jiggler) {
        const j = hid.result.jiggler;
        $ha("ha-jiggler-switch").checked = !!j.active;
        $ha("ha-jiggler-row").classList.toggle("ha-disabled", !j.enabled);
      }
    } catch (e) {
      // Native jiggler status is a convenience add-on; ignore transient failures.
    }
  }

  function startPolling() {
    stopPolling();
    tick();
    pollTimer = window.setInterval(tick, 2000);
  }

  function stopPolling() {
    if (pollTimer !== null) {
      window.clearInterval(pollTimer);
      pollTimer = null;
    }
  }

  function closeModal() {
    const el = document.getElementById(MODAL_ID);
    if (el) el.remove();
    stopPolling();
    document.removeEventListener("keydown", onKeydown);
  }

  function onKeydown(ev) {
    if (ev.code === "Escape") closeModal();
  }

  function toggleModal() {
    if (document.getElementById(MODAL_ID)) {
      closeModal();
      return;
    }
    openModal();
  }

  function openModal() {
    injectStyles();
    draftEvents = [];

    const modal = document.createElement("div");
    modal.className = "modal";
    modal.id = MODAL_ID;
    modal.innerHTML = `
      <div class="modal-window">
        <div class="modal-header">
          <span>HID Automation</span>
          <button class="ha-close" title="Schließen">&#10005;</button>
        </div>
        <div class="modal-content">${contentHtml()}</div>
      </div>
    `;

    modal.addEventListener("mousedown", (ev) => {
      if (ev.target === modal) closeModal();
    });
    modal.querySelector(".ha-close").addEventListener("click", closeModal);

    modal.querySelector("#ha-trigger").addEventListener("click", () => api("/trigger", {}).then(tick).catch(showModalErr));
    modal.querySelector("#ha-auto-save").addEventListener("click", () => api("/settings", {
      auto: $ha("ha-auto-switch").checked,
      autoMinMinutes: +$ha("ha-amin").value,
      autoMaxMinutes: +$ha("ha-amax").value,
    }).then(tick).catch(showModalErr));

    modal.querySelector("#ha-jiggler-switch").addEventListener("change", (ev) => {
      const enabled = ev.target.checked;
      kvmdPost(`/api/hid/set_params?jiggler=${enabled}`).then((j) => {
        if (!j.ok) {
          ev.target.checked = !enabled;
          showModalErr(new Error("Mouse Jiggler konnte nicht umgeschaltet werden"));
        }
      }).catch(() => { ev.target.checked = !enabled; });
    });

    modal.querySelector("#ha-click-capture").addEventListener("click", () => {
      if (lastAbsPos) {
        $ha("ha-clickx").value = lastAbsPos.x;
        $ha("ha-clicky").value = lastAbsPos.y;
      } else {
        showModalErr(new Error("Noch keine Mausposition erfasst — Maus im Mouse-Fenster über den Stream bewegen."));
      }
    });
    modal.querySelector("#ha-click-save").addEventListener("click", () => api("/settings", {
      clickX: $ha("ha-clickx").value === "" ? null : +$ha("ha-clickx").value,
      clickY: $ha("ha-clicky").value === "" ? null : +$ha("ha-clicky").value,
    }).then(tick).catch(showModalErr));
    modal.querySelector("#ha-click-clear").addEventListener("click", () => {
      $ha("ha-clickx").value = "";
      $ha("ha-clicky").value = "";
      api("/settings", {clickX: null, clickY: null}).then(tick).catch(showModalErr);
    });

    modal.querySelector("#ha-calc").addEventListener("click", calculatePlan);
    modal.querySelector("#ha-plan-start").addEventListener("click", startPlan);
    modal.querySelector("#ha-plan-stop").addEventListener("click", stopPlan);

    document.body.appendChild(modal);
    setDefaultStart();
    document.addEventListener("keydown", onKeydown);
    startPolling();
  }

  // ---- mouse coordinate readout in PiKVM's native Mouse window ---------
  //
  // PiKVM computes the absolute HID position (-32768..32767, remapped from
  // the pointer's pixel position over the video frame) purely internally in
  // mouse.js and never displays it. We can't reuse that internal state (it's
  // module-scoped, not exposed on window), so this listens on the same
  // #stream-box element independently (DOM elements allow multiple
  // listeners) and reproduces the identical geometry math from stream.js's
  // getGeometry(), sourced from the #stream-image element's natural size
  // instead of PiKVM's internal streamer object.
  const COORDS_ID = "hid-automation-coords";

  function computeMousePosition(clientX, clientY) {
    const streamBox = document.getElementById("stream-box");
    const img = document.getElementById("stream-image");
    if (!streamBox || !img) return null;
    const realW = img.naturalWidth || img.videoWidth;
    const realH = img.naturalHeight || img.videoHeight;
    if (!realW || !realH) return null;
    const rect = streamBox.getBoundingClientRect();
    const ratio = Math.min(rect.width / realW, rect.height / realH);
    const geoX = Math.round((rect.width - ratio * realW) / 2);
    const geoY = Math.round((rect.height - ratio * realH) / 2);
    const geoWidth = Math.round(ratio * realW);
    const geoHeight = Math.round(ratio * realH);
    const px = Math.max(Math.round(clientX - rect.left), 0);
    const py = Math.max(Math.round(clientY - rect.top), 0);
    const remap = (v, inMin, inMax, outMin, outMax) => (
      Math.round(outMin + (outMax - outMin) * (v - inMin) / (inMax - inMin))
    );
    return {
      px, py,
      absX: remap(px - geoX, 0, geoWidth - 1, -32768, 32767),
      absY: remap(py - geoY, 0, geoHeight - 1, -32768, 32767),
    };
  }

  function addMouseCoordsDisplay() {
    if (document.getElementById(COORDS_ID)) return;

    const win = document.getElementById("mouse-window");
    const buttons = document.getElementById("mouse-buttons");
    const streamBox = document.getElementById("stream-box");
    if (!win || !buttons || !streamBox) {
      window.setTimeout(addMouseCoordsDisplay, 250);
      return;
    }

    const box = document.createElement("div");
    box.id = COORDS_ID;
    box.style.cssText = (
      "padding:4px 8px;font-size:12px;text-align:center;" +
      "color:var(--cs-window-header-default-fg,#aaa);" +
      "border-bottom:var(--border-default-thin,1px solid #36393f)"
    );
    box.textContent = "Pixel: — · Absolut: —";
    buttons.insertAdjacentElement("beforebegin", box);

    let lastUpdate = 0;
    streamBox.addEventListener("mousemove", (ev) => {
      const now = Date.now();
      if (now - lastUpdate < 50) return; // throttle to ~20fps, no need for every event
      lastUpdate = now;
      const pos = computeMousePosition(ev.clientX, ev.clientY);
      if (pos) {
        box.textContent = `Pixel: ${pos.px}, ${pos.py} · Absolut: ${pos.absX}, ${pos.absY}`;
        lastAbsPos = {x: pos.absX, y: pos.absY};
      }
    });
    streamBox.addEventListener("mouseleave", () => {
      box.textContent = "Pixel: — · Absolut: —";
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      addNavbarItem();
      addMouseCoordsDisplay();
    }, {once: true});
  } else {
    addNavbarItem();
    addMouseCoordsDisplay();
  }
})();
