#pragma once

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#10151d">
  <title>Govee Sensoren</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #10151d;
      color: #eef3f8;
      font-synthesis: none;
      text-rendering: optimizeLegibility;
    }
    * { box-sizing: border-box; }
    body {
      min-width: 320px;
      min-height: 100vh;
      margin: 0;
      background:
        radial-gradient(ellipse at 50% -20%, rgba(35, 91, 126, .28), transparent 55%),
        #10151d;
    }
    main { width: min(1120px, 100%); margin: 0 auto; padding: 46px 24px 64px; }
    header { display: flex; justify-content: space-between; align-items: end; gap: 20px; margin-bottom: 30px; }
    .eyebrow { margin: 0 0 9px; color: #64d9c0; font-size: 11px; font-weight: 750; letter-spacing: .2em; text-transform: uppercase; }
    h1 { margin: 0; font-size: clamp(28px, 5vw, 42px); letter-spacing: -.045em; }
    .subtitle { margin: 9px 0 0; color: #8995a5; font-size: 14px; }
    #refresh { color: #778393; font-size: 12px; white-space: nowrap; }
    #sensors { display: grid; grid-template-columns: repeat(auto-fit, minmax(245px, 1fr)); gap: 18px; }
    .card, .empty {
      border: 1px solid rgba(151, 170, 190, .12);
      border-radius: 20px;
      background: linear-gradient(145deg, rgba(33, 42, 54, .96), rgba(24, 31, 41, .96));
      box-shadow: 0 18px 50px rgba(0, 0, 0, .18);
    }
    .card { padding: 20px 20px 18px; text-align: center; }
    .device-title-row { display: flex; justify-content: center; align-items: center; gap: 5px; min-width: 0; }
    .device-name { overflow: hidden; min-width: 0; margin: 0; padding: 0; border: 0; background: none; color: #eaf0f6; font: inherit; font-size: 14px; font-weight: 650; text-overflow: ellipsis; white-space: nowrap; cursor: pointer; }
    .device-name:hover { color: #64d9c0; }
    .alias-remove { width: 18px; height: 18px; padding: 0; border: 1px solid rgba(151, 170, 190, .25); border-radius: 50%; background: transparent; color: #8793a1; font-size: 13px; line-height: 15px; cursor: pointer; }
    .alias-remove:hover { border-color: #ef8f82; color: #ffad9f; }
    .device-address { margin: 5px 0 2px; color: #758293; font: 11px ui-monospace, SFMono-Regular, Consolas, monospace; }
    .gauge { display: block; width: min(100%, 236px); height: auto; margin: 4px auto 0; overflow: visible; }
    .track { fill: none; stroke: rgba(144, 161, 180, .14); stroke-linecap: round; }
    .temperature-arc, .humidity-arc { fill: none; stroke-linecap: round; }
    .temperature { fill: #f1f5fa; font-size: 32px; font-weight: 700; letter-spacing: -.055em; text-anchor: middle; }
    .temperature-unit { fill: #9da9b8; font-size: 11px; font-weight: 600; }
    .humidity { fill: #aab5c2; font-size: 12px; font-weight: 550; text-anchor: middle; }
    .scale-label { fill: #7d8998; font-size: 9px; text-anchor: middle; }
    .battery-outline { fill: none; stroke: #8e9ba9; stroke-width: 1.5; }
    .battery-value { fill: #9ca8b6; font-size: 9px; font-weight: 600; text-anchor: middle; }
    .last-seen { margin: 0; color: #788493; font-size: 11px; }
    .empty { grid-column: 1 / -1; padding: 42px 24px; color: #95a1af; text-align: center; }
    .empty strong { display: block; margin-bottom: 8px; color: #dce5ed; font-size: 16px; }
    .error { color: #ffad83; }
    @media (max-width: 520px) {
      main { padding: 32px 16px 48px; }
      header { align-items: start; flex-direction: column; gap: 10px; }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <p class="eyebrow">Govee · Live-Monitor</p>
        <h1>Raumklima</h1>
        <p class="subtitle">Temperatur, Luftfeuchte und Batteriestand deiner Sensoren</p>
      </div>
      <div id="refresh" aria-live="polite">Warte auf Sensordaten …</div>
    </header>
    <section id="sensors" aria-live="polite"></section>
  </main>
  <script>
    const sensorGrid = document.getElementById("sensors");
    const refreshLabel = document.getElementById("refresh");
    const svgNamespace = "http://www.w3.org/2000/svg";

    function svgElement(tag, attributes = {}) {
      const element = document.createElementNS(svgNamespace, tag);
      for (const [name, value] of Object.entries(attributes)) element.setAttribute(name, value);
      return element;
    }

    function arcPath(radius, progress) {
      const start = 135;
      const end = start + Math.max(0, Math.min(1, progress)) * 270;
      const point = angle => {
        const radians = angle * Math.PI / 180;
        return [100 + radius * Math.cos(radians), 100 + radius * Math.sin(radians)];
      };
      const [x1, y1] = point(start);
      const [x2, y2] = point(end);
      return `M ${x1} ${y1} A ${radius} ${radius} 0 ${end - start > 180 ? 1 : 0} 1 ${x2} ${y2}`;
    }

    function makeGauge(sensor, index) {
      const temperature = Number(sensor.temperature_c);
      const humidity = Number(sensor.humidity_percent);
      const battery = Number(sensor.battery_percent);
      const card = document.createElement("article");
      card.className = "card";

      const titleRow = document.createElement("div");
      titleRow.className = "device-title-row";
      const name = document.createElement("button");
      name.type = "button";
      name.className = "device-name";
      name.textContent = sensor.alias || sensor.name || "Govee-Sensor";
      name.title = "Antippen, um den Alias zu ändern";
      name.addEventListener("click", () => editAlias(sensor));
      titleRow.append(name);
      if (sensor.alias) {
        const removeAlias = document.createElement("button");
        removeAlias.type = "button";
        removeAlias.className = "alias-remove";
        removeAlias.textContent = "×";
        removeAlias.setAttribute("aria-label", `Alias ${sensor.alias} entfernen`);
        removeAlias.title = "Alias entfernen";
        removeAlias.addEventListener("click", () => saveAlias(sensor, ""));
        titleRow.append(removeAlias);
      }
      const address = document.createElement("p");
      address.className = "device-address";
      address.textContent = sensor.address || "";
      card.append(titleRow, address);

      const gauge = svgElement("svg", { class: "gauge", viewBox: "0 0 200 208", role: "img" });
      gauge.setAttribute("aria-label", `${temperature.toFixed(1)} Grad Celsius, ${humidity.toFixed(1)} Prozent Luftfeuchte, Batterie ${battery} Prozent`);
      const definitions = svgElement("defs");
      const gradient = svgElement("linearGradient", { id: `temperature-gradient-${index}`, x1: "0", y1: "1", x2: "1", y2: "0" });
      gradient.append(
        svgElement("stop", { offset: "0%", "stop-color": "#4c9ef5" }),
        svgElement("stop", { offset: "52%", "stop-color": "#5bd3bd" }),
        svgElement("stop", { offset: "100%", "stop-color": "#f0ac62" })
      );
      definitions.append(gradient);
      gauge.append(definitions);
      gauge.append(
        svgElement("path", { class: "track", d: arcPath(78, 1), "stroke-width": "9" }),
        svgElement("path", { class: "temperature-arc", d: arcPath(78, (temperature - 12) / 16), stroke: `url(#temperature-gradient-${index})`, "stroke-width": "9" }),
        svgElement("path", { class: "track", d: arcPath(64, 1), "stroke-width": "5" }),
        svgElement("path", {
          class: "humidity-arc",
          d: arcPath(64, humidity / 100),
          stroke: humidity >= 40 && humidity <= 60 ? "#51d5a5" : "#f2a34f",
          "stroke-width": "5"
        })
      );

      const temperatureText = svgElement("text", { class: "temperature", x: "100", y: "96" });
      temperatureText.textContent = temperature.toFixed(1);
      const unit = svgElement("tspan", { class: "temperature-unit" });
      unit.textContent = " °C";
      temperatureText.append(unit);
      const humidityText = svgElement("text", { class: "humidity", x: "100", y: "119" });
      humidityText.textContent = `${humidity.toFixed(1)} % Luftfeuchte`;
      gauge.append(temperatureText, humidityText);

      const lowerBound = svgElement("text", { class: "scale-label", x: "32", y: "160" });
      lowerBound.textContent = "12°";
      const upperBound = svgElement("text", { class: "scale-label", x: "168", y: "160" });
      upperBound.textContent = "28°";
      gauge.append(lowerBound, upperBound);

      const batteryColor = battery <= 20 ? "#f07869" : battery <= 40 ? "#f0a34f" : "#56d3a3";
      gauge.append(
        svgElement("rect", { class: "battery-outline", x: "78", y: "160", width: "40", height: "19", rx: "3" }),
        svgElement("rect", { class: "battery-outline", x: "118", y: "165", width: "4", height: "9", rx: "1" })
      );
      for (let block = 0; block < 4; block++) {
        const blockAttributes = {
          class: "battery-block",
          x: String(82 + block * 8),
          y: "164",
          width: "5",
          height: "11",
          rx: "1",
          fill: block < Math.ceil(battery / 25) ? batteryColor : "#4a5664"
        };
        gauge.append(svgElement("rect", blockAttributes));
      }
      const batteryText = svgElement("text", { class: "battery-value", x: "100", y: "192" });
      batteryText.textContent = `${battery} % Batterie`;
      gauge.append(batteryText);
      card.append(gauge);

      const lastSeen = document.createElement("p");
      lastSeen.className = "last-seen";
      lastSeen.textContent = sensor.last_seen_seconds < 10
        ? "Gerade empfangen"
        : `Zuletzt vor ${sensor.last_seen_seconds} s empfangen`;
      card.append(lastSeen);
      return card;
    }

    async function saveAlias(sensor, alias) {
      try {
        const response = await fetch("/api/aliases", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ address: sensor.address, alias })
        });
        if (!response.ok) {
          const result = await response.json();
          const message = result.error === "alias_limit_reached"
            ? "Es können höchstens 8 Geräte einen Alias erhalten."
            : result.error === "alias_too_long"
              ? "Der Alias darf höchstens 20 Zeichen lang sein."
              : "Der Alias konnte nicht gespeichert werden.";
          throw new Error(message);
        }
        await refreshSensors();
      } catch (error) {
        window.alert(error.message);
        console.error("Sensoralias konnte nicht gespeichert werden:", error);
      }
    }

    async function editAlias(sensor) {
      const value = window.prompt("Alias (max. 20 Zeichen):", sensor.alias || "");
      if (value === null) return;
      const alias = value.trim();
      if (Array.from(alias).length > 20) {
        window.alert("Der Alias darf höchstens 20 Zeichen lang sein.");
        return;
      }
      if (alias) await saveAlias(sensor, alias);
    }

    function showEmpty(message, detail = "") {
      const empty = document.createElement("div");
      empty.className = "empty";
      const title = document.createElement("strong");
      title.textContent = message;
      empty.append(title);
      if (detail) empty.append(document.createTextNode(detail));
      sensorGrid.replaceChildren(empty);
    }

    async function refreshSensors() {
      try {
        const response = await fetch("/api/sensors", { cache: "no-store" });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        if (!Array.isArray(data.sensors) || data.sensors.length === 0) {
          showEmpty("Noch keine Sensordaten", "Sobald ein Govee-Sensor empfangen wurde, erscheint er hier.");
        } else {
          sensorGrid.replaceChildren(...data.sensors.map(makeGauge));
        }
        refreshLabel.textContent = `Aktualisiert: ${new Date().toLocaleTimeString("de-DE")}`;
        refreshLabel.classList.remove("error");
      } catch (error) {
        refreshLabel.textContent = "Verbindung zum Sensor-Board fehlgeschlagen";
        refreshLabel.classList.add("error");
        if (!sensorGrid.childElementCount) showEmpty("Dashboard nicht erreichbar", "Bitte Verbindung zum ESP32 prüfen.");
        console.error("Sensordaten konnten nicht geladen werden:", error);
      }
    }

    refreshSensors();
    window.setInterval(refreshSensors, 5000);
  </script>
</body>
</html>
)HTML";
