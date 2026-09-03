(() => {
  "use strict";

  const ITEM_ID = "hid-automation-navbar";

  function rootPrefix() {
    const path = window.location.pathname;
    const match = path.match(/^(.*\/)(?:kvm\/?)(?:index\.html)?$/);
    return match ? match[1] : "/";
  }

  function addNavbarItem() {
    if (document.getElementById(ITEM_ID)) return;

    const navbar = document.getElementById("navbar");
    const macro = document.getElementById("macro-dropdown");
    if (!navbar || !macro) {
      window.setTimeout(addNavbarItem, 250);
      return;
    }

    const root = rootPrefix();
    const item = document.createElement("li");
    item.className = "right";
    item.id = ITEM_ID;

    const link = document.createElement("a");
    link.className = "menu-item menu-action";
    link.href = `${root}hid-automation/`;
    link.title = "HID Automation öffnen";
    link.setAttribute("aria-label", "HID Automation öffnen");

    const icon = document.createElement("img");
    icon.className = "svg-gray";
    icon.src = `${root}share/svg/led-gear.svg`;
    icon.alt = "";

    const label = document.createElement("span");
    label.textContent = " Automation";

    link.append(icon, label);
    item.appendChild(link);

    // PiKVM uses right-floating navbar items. Inserting after Macro places
    // Automation beside the existing Macro/Text controls without touching
    // PiKVM's own HTML or JavaScript files.
    macro.insertAdjacentElement("afterend", item);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", addNavbarItem, {once: true});
  } else {
    addNavbarItem();
  }
})();
