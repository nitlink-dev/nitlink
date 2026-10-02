    // Localization resources are external so a new language can be added
    // without rewriting the UI. Missing keys always resolve through en-US.
    const localeResources = window.NitLinkLocales || {};
    const englishLocale = localeResources['en-US'] || {};
    const hasWebViewHost = () => !!(window.chrome && window.chrome.webview &&
      window.chrome.webview.postMessage);
    const detectStandaloneLocale = () => {
      const locales = (window.navigator && window.navigator.languages && window.navigator.languages.length)
        ? window.navigator.languages
        : [window.navigator && window.navigator.language];
      return locales.some(locale => typeof locale === 'string' &&
        ['zh-tw', 'zh-hant-tw'].includes(locale.toLowerCase()))
        ? 'zh-TW' : 'en-US';
    };
    const standaloneLanguageKey = 'nitlink.languagePreference';
    const standalonePreference = (() => {
      if (hasWebViewHost()) return 'system';
      try {
        const saved = window.localStorage.getItem(standaloneLanguageKey);
        return ['system', 'en-US', 'zh-TW'].includes(saved) ? saved : 'system';
      } catch (_) {
        return 'system';
      }
    })();
    let currentLocale = 'en-US';
    const t = key => {
      const selected = localeResources[currentLocale] || {};
      const value = selected[key] || englishLocale[key];
      return (typeof value === 'string' && value.length > 0) ? value : key;
    };
    const applyTranslations = locale => {
      currentLocale = localeResources[locale] ? locale : 'en-US';
      document.documentElement.lang = currentLocale;
      document.querySelectorAll('[data-i18n]').forEach(el => {
        el.textContent = t(el.dataset.i18n);
      });
      document.querySelectorAll('[data-i18n-title]').forEach(el => {
        el.title = t(el.dataset.i18nTitle);
      });
      document.querySelectorAll('[data-i18n-aria-label]').forEach(el => {
        el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel));
      });
      const active = document.querySelector('.rail-item[data-active="true"]');
      const titleKey = active && ({
        'pane-video': 'top.settings',
        'pane-audio': 'top.settings',
        'pane-shortcuts': 'section.shortcuts',
        'pane-about': 'section.about'
      })[active.dataset.tab];
      const h1 = document.querySelector('.topbar h1');
      if (h1 && titleKey) h1.textContent = t(titleKey);
      const language = document.getElementById('language-select');
      if (language && language.value !== 'system' && language.value !== 'en-US' && language.value !== 'zh-TW') {
        language.value = 'system';
      }
    };
    const localizeValue = value => ({
      Auto: 'value.auto', Stretch: 'value.stretch',
      Right: 'value.right', Left: 'value.left', Full: 'value.full'
    }[value] ? t({
      Auto: 'value.auto', Stretch: 'value.stretch',
      Right: 'value.right', Left: 'value.left', Full: 'value.full'
    }[value]) : value);
    // A real NitLink host will immediately push its effective locale. When
    // this file is opened directly for static testing, use the browser-local
    // preference instead so the language selector still has visible effect.
    const standaloneLocale = standalonePreference === 'zh-TW' ? 'zh-TW'
      : standalonePreference === 'en-US' ? 'en-US' : detectStandaloneLocale();
    applyTranslations(standaloneLocale);

    // Rail navigation. Every pane stays in the DOM so applyState can keep
    // writing into hidden ones; only the active flag flips.
    (() => {
      const tabs = [...document.querySelectorAll('.rail-item[data-tab]')];
      const titles = {
        'pane-video': 'top.settings', 'pane-audio': 'top.settings',
        'pane-shortcuts': 'section.shortcuts', 'pane-about': 'section.about'
      };
      const h1 = document.querySelector('.topbar h1');
      tabs.forEach(tab => tab.addEventListener('click', () => {
        tabs.forEach(other => {
          const pane = document.getElementById(other.dataset.tab);
          const active = (other === tab);
          if (active) other.dataset.active = 'true'; else other.removeAttribute('data-active');
          if (pane) { if (active) pane.dataset.active = 'true'; else pane.removeAttribute('data-active'); }
        });
        if (h1 && titles[tab.dataset.tab]) h1.textContent = t(titles[tab.dataset.tab]);
      }));
    })();

    const post = (action, value) => {
      const payload = (value === undefined) ? { action } : { action, value };
      if (hasWebViewHost()) {
        window.chrome.webview.postMessage(payload);
      }
    };

    const languageSelect = document.getElementById('language-select');
    if (languageSelect) {
      if (!hasWebViewHost()) languageSelect.value = standalonePreference;
      languageSelect.addEventListener('change', () => {
        const preference = languageSelect.value;
        applyTranslations(preference === 'zh-TW' ? 'zh-TW'
          : preference === 'en-US' ? 'en-US' : detectStandaloneLocale());
        if (!hasWebViewHost()) {
          try { window.localStorage.setItem(standaloneLanguageKey, preference); } catch (_) {}
        }
        post('setLanguage', preference);
      });
    }

    document.addEventListener('click', e => {
      let actionEl = e.target.closest('[data-action]');
      if (!actionEl || actionEl.classList.contains('slider')) return;
      const action = actionEl.dataset.action;
      if (actionEl.querySelector('.toggle') || actionEl.classList.contains('toggle')) {
        const toggleEl = actionEl.querySelector('.toggle') || actionEl;
        const willBeOn = !toggleEl.classList.contains('on');
        toggleEl.classList.toggle('on', willBeOn);
        post(action, willBeOn);
        return;
      }
      post(action);
    });

    (() => {
      const slider = document.getElementById('slider-volume');
      const fill = document.getElementById('slider-volume-fill');
      const thumb = document.getElementById('slider-volume-thumb');
      const valEl = document.getElementById('slider-volume-val');
      const setVisual = pct => {
        const clamped = Math.max(0, Math.min(100, pct));
        fill.style.width = clamped + '%';
        thumb.style.left = clamped + '%';
        valEl.textContent = Math.round(clamped);
      };
      const onPointer = e => {
        const rect = slider.getBoundingClientRect();
        const pct = ((e.clientX - rect.left) / rect.width) * 100;
        setVisual(pct);
        post('setVolume', Math.max(0, Math.min(100, pct)) / 100);
      };
      let dragging = false;
      slider.addEventListener('pointerdown', e => { dragging = true; slider.setPointerCapture(e.pointerId); onPointer(e); });
      slider.addEventListener('pointermove', e => { if (dragging) onPointer(e); });
      slider.addEventListener('pointerup', e => { dragging = false; slider.releasePointerCapture(e.pointerId); });
      slider._setFromCpp = setVisual;
    })();

    const opacitySlider = document.getElementById('slider-pip-opacity');
    const opacityValue = document.getElementById('slider-pip-opacity-val');
    const setPiPOpacityVisual = opacity => {
      if (!Number.isFinite(opacity) || opacitySlider.matches(':active')) return;
      const pct = Math.round(Math.max(10, Math.min(100, opacity * 100)));
      opacitySlider.value = pct;
      opacityValue.textContent = pct + '%';
    };
    opacitySlider.addEventListener('input', () => {
      const pct = Number(opacitySlider.value);
      opacityValue.textContent = pct + '%';
      post('setPiPOpacity', pct / 100);
    });

    // Capture source picker + manual format override.
    //
    // The popover hosts two related pieces: a device list (only rendered
    // when more than one capture device is enumerated) and a three-axis
    // manual format override (always rendered). Resolution, Framerate
    // and Format cascade: each drives the option set for the one below
    // it, with "Auto" at index 0 mapping to the empty-value wildcard.
    //
    // State is push-only from C++: applyState forwards the latest
    // devices / active / availableFormats / captureFormatOverride into
    // _updateSource. The popover holds no optimistic state. A failed
    // override (combination unavailable for the live source) reverts
    // visually as soon as the next PushSettingsState arrives with
    // Auto values and the toast notification.
    (() => {
      const trigger = document.getElementById('source-trigger');
      const nameEl = document.getElementById('meta-source');
      const pop = document.getElementById('source-popover');
      const hdmiPicker = document.getElementById('hdmi-source-picker');
      if (!trigger || !nameEl || !pop) return;

      let lastDevices = [];
      let lastActive = '';
      // Available formats from the live capture device, already filtered
      // upstream to skip interlaced entries. Each: { width, height, fps,
      // format }. Empty until the first state push after Open().
      let lastAvailableFormats = [];
      // User's persisted override. Each numeric field at 0 means Auto for
      // that axis; empty format string means Auto for format. All-Auto =
      // full automatic negotiation (the default).
      let lastOverride = {
        width: 0, height: 0, fps: 0,
        fpsNumerator: 0, fpsDenominator: 1, format: ''
      };

      const closePop = () => { pop.dataset.open = 'false'; };

      // Build one labelled <select> row. The Auto option always sits at
      // index 0 with empty value so the cascade's wildcard semantics map
      // directly to falsy values in the change handler; no special
      // casing for "Auto" needed.
      let sourceSelects = {};
      const updateSelect = (sel, options, currentValue) => {
        sel.innerHTML = '';
        [{ value: '', label: t('value.auto') }, ...options].forEach(o => {
          const opt = document.createElement('option');
          opt.value = String(o.value);
          opt.textContent = o.label;
          sel.appendChild(opt);
        });
        sel.value = currentValue;
      };
      const makeSelectRow = (labelKey, options, currentValue, onChange) => {
        const row = document.createElement('div');
        row.className = 'source-popover-select-row';
        const lbl = document.createElement('label');
        lbl.textContent = t(labelKey);
        const sel = document.createElement('select');
        sel.className = 'source-popover-select';

        sourceSelects[labelKey] = sel;
        updateSelect(sel, options, currentValue);

        // Setting .value to a string not present in options falls back to
        // the first option (Auto), which is the correct UX when the
        // persisted override has a dimension that is no longer available
        // (e.g. source changed and the old fps is gone for this res).
        sel.value = currentValue;
        // Block the popover's click-outside-close logic while the user
        // is interacting with the native <select> dropdown. Without
        // this, clicking the control fires the document-level handler
        // below and closes the popover before the user can pick an
        // option.
        sel.addEventListener('click', ev => ev.stopPropagation());
        sel.addEventListener('change', ev => {
          ev.stopPropagation();
          onChange(ev.target.value);
        });
        row.appendChild(lbl);
        row.appendChild(sel);
        pop.appendChild(row);
      };

      // The renderer supports three pixel formats end-to-end:
      //   P010 (HDR10), NV12 (SDR YUV), BGRA (SDR RGB).
      // YUY2 / UYVY / RGB24 / ARGB32 are exposed by some capture cards
      // (the 4K Pro publishes YUY2 at 1080p exclusively, for example)
      // but routing them through the renderer would require shader
      // paths the renderer does not implement. Filter the cascade
      // strictly to renderable formats so the user is never offered a
      // combination that would fail with a confusing toast.
      const RENDERABLE = new Set(['P010', 'NV12', 'BGRA']);

      // Cascade helpers. Each derives its set from lastAvailableFormats
      // filtered by the upstream override selections AND by renderable-
      // format gating above. Picking Auto on any axis treats that axis
      // as wildcard for the levels below. A resolution / fps that has
      // no renderable format on the live device does not appear in the
      // dropdown at all; the device's full enumeration is still in
      // lastAvailableFormats (the [NitLink/Formats] log shows everything)
      // but the UI is honest about what NitLink can actually deliver.
      const uniqueResolutions = () => {
        const map = new Map();
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          map.set(`${f.width}x${f.height}`, { w: f.width, h: f.height });
        });
        return [...map.entries()].sort((a, b) => b[1].w - a[1].w);
      };
      const rateKey = (format) => {
        const numerator = Number(format && format.fpsNumerator);
        const denominator = Number(format && format.fpsDenominator);
        if (numerator > 0 && denominator > 0) return `${numerator}/${denominator}`;
        const fps = Number(format && format.fps);
        return fps > 0 ? `${fps}/1` : '0/1';
      };
      const rateLabel = (format) => {
        const numerator = Number(format && format.fpsNumerator);
        const denominator = Number(format && format.fpsDenominator);
        let fps = Number(format && format.fps) || 0;
        if (numerator > 0 && denominator > 0) {
          const exact = numerator / denominator;
          const roundedInteger = Math.round(exact);
          fps = Math.abs(exact - roundedInteger) < 0.01
            ? roundedInteger
            : Number(exact.toFixed(3));
        }
        return `${fps} ${t('units.fps')}`;
      };
      const overrideRateKey = () => {
        const direct = rateKey(lastOverride);
        const legacyFps = Number(lastOverride && lastOverride.fps);
        const legacyNumerator = Number(lastOverride && lastOverride.fpsNumerator);
        if (!(legacyFps > 0) || legacyNumerator > 0) return direct;

        // Integer-only persisted preferences predate rational-rate storage.
        // Resolve their display key against the live native list without
        // mutating lastOverride, so 59 can select 60000/1001 in the picker
        // while the C++ negotiator still owns the authoritative migration.
        const w = Number(lastOverride.width) || 0;
        const h = Number(lastOverride.height) || 0;
        const format = typeof lastOverride.format === 'string'
          ? lastOverride.format : '';
        const candidates = lastAvailableFormats.filter(f =>
          RENDERABLE.has(f.format) &&
          (!w || f.width === w) &&
          (!h || f.height === h) &&
          Number(f.fps) === legacyFps &&
          (!format || f.format === format));
        if (!candidates.length) return direct;

        candidates.sort((a, b) => {
          const aRate = Number(a.fpsNumerator || a.fps) /
            Number(a.fpsDenominator || 1);
          const bRate = Number(b.fpsNumerator || b.fps) /
            Number(b.fpsDenominator || 1);
          return bRate - aRate;
        });
        return rateKey(candidates[0]);
      };
      const uniqueFps = () => {
        // Strict filter: fps options appear only when a resolution is
        // selected AND a renderable format exists at that (resolution,
        // fps) combo on the current device. When Resolution=Auto,
        // returns empty so the user picks resolution first. Prevents
        // the cascade from offering fps that are not achievable at
        // the chosen resolution.
        const set = new Map();
        const w = lastOverride.width, h = lastOverride.height;
        if (!w || !h) return [];
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          if (f.width === w && f.height === h) set.set(rateKey(f), f);
        });
        return [...set.values()].sort((a, b) =>
          b.fps - a.fps || Number(b.fpsNumerator || b.fps) - Number(a.fpsNumerator || a.fps));
      };
      const uniqueFormats = () => {
        // Strict filter: format options appear only when a resolution
        // is selected. When Resolution=Auto, returns empty so the user
        // picks resolution first.
        const set = new Set();
        const w = lastOverride.width, h = lastOverride.height;
        const selectedRate = overrideRateKey();
        if (!w || !h) return [];
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          if (f.width === w && f.height === h) {
            if (selectedRate === '0/1' || rateKey(f) === selectedRate) set.add(f.format);
          }
        });
        return [...set];
      };

      // POST the override back to C++. The backend persists into Config
      // and triggers a format reconcile. If the combo is unavailable,
      // the backend falls back to Auto and the next state push carries
      // state.notification = "Capture format unavailable, ..." which
      // surfaces a toast via showToast() in applyState.
      const refreshSourceSelects = () => {
        const resolutionSelected =
          lastOverride.width > 0 && lastOverride.height > 0;
        const choices = {
            'source.resolution': [uniqueResolutions().map(([key]) => ({ value: key, label: key })),
            lastOverride.width ? `${lastOverride.width}x${lastOverride.height}` : ''],
            'source.framerate': [uniqueFps().map(f => ({ value: rateKey(f), label: rateLabel(f) })),
            overrideRateKey() === '0/1' ? '' : overrideRateKey()],
          'source.format': [uniqueFormats().map(f => ({ value: f, label: f })), lastOverride.format],
        };
        Object.entries(choices).forEach(([label, [options, value]]) => {
          const sel = sourceSelects[label];
          // Keep an open native dropdown intact; its dependent controls can
          // still follow the new resolution/FPS without replacing the row.
          if (sel && sel !== document.activeElement) updateSelect(sel, options, value);
        });
        ['source.framerate', 'source.format'].forEach(label => {
          const sel = sourceSelects[label];
          if (sel) sel.disabled = !resolutionSelected;
        });
      };
      const sendOverride = (ov) => {
        lastOverride = ov;
        refreshSourceSelects();
        post('setCaptureFormatOverride', ov);
      };

      const renderPopover = () => {
        // Keep the identity picker mounted with its listeners, selection and
        // custom draft when device/format state refreshes the original menu.
        pop.replaceChildren();
        sourceSelects = {};

        // Device list: only when there is more than one to pick from.
        if (lastDevices.length > 1) {
          const sectLabel = document.createElement('div');
          sectLabel.className = 'source-popover-section-label';
          sectLabel.textContent = t('source.captureDevice');
          pop.appendChild(sectLabel);

          lastDevices.forEach(name => {
            const item = document.createElement('div');
            item.className = 'source-popover-item';
            if (name === lastActive) item.dataset.active = 'true';
            const dot = document.createElement('span');
            dot.className = 'source-popover-bullet';
            const lbl = document.createElement('span');
            lbl.className = 'source-popover-name';
            lbl.textContent = name;
            item.appendChild(dot);
            item.appendChild(lbl);
            item.addEventListener('click', ev => {
              ev.stopPropagation();
              post('setPreferredDevice', name);
              closePop();
            });
            pop.appendChild(item);
          });

          const divider = document.createElement('div');
          divider.className = 'source-popover-divider';
          pop.appendChild(divider);
        }

        // Manual capture format. Always rendered so single-device users
        // can still pick a resolution / framerate / format different
        // from auto-negotiation.
        const fmtSectLabel = document.createElement('div');
        fmtSectLabel.className = 'source-popover-section-label';
        fmtSectLabel.textContent = t('source.manualFormat');
        pop.appendChild(fmtSectLabel);
        pop.appendChild(hdmiPicker);

        const resolutions = uniqueResolutions();
        const currentResValue = lastOverride.width
          ? `${lastOverride.width}x${lastOverride.height}` : '';
        makeSelectRow('source.resolution',
          resolutions.map(([key]) => ({ value: key, label: key })),
          currentResValue,
          (newVal) => {
            const [w, h] = newVal ? newVal.split('x').map(Number) : [0, 0];
            // Resolution=Auto resets the whole override. Partial
            // overrides (native-best resolution with a specific
            // fps/format) are rarely supported by real hardware.
            if (!w || !h) {
              sendOverride({ width: 0, height: 0, fps: 0,
                fpsNumerator: 0, fpsDenominator: 1, format: '' });
              return;
            }
            // Stale-child invalidation: if the persisted fps or format
            // is no longer a valid combination at the newly selected
            // resolution, reset that axis to Auto. Prevents the cascade
            // from sending an override the current device cannot
            // satisfy (e.g. switching from 1080p+240fps to 4K when
            // 4K@240 does not exist).
            const validAtNewRes = lastAvailableFormats.filter(f =>
              RENDERABLE.has(f.format) &&
              f.width === w && f.height === h);
            const selectedRate = overrideRateKey();
            const fpsStillValid = selectedRate !== '0/1' &&
              validAtNewRes.some(f => rateKey(f) === selectedRate);
            const fmtStillValid = lastOverride.format &&
              validAtNewRes.some(f => f.format === lastOverride.format);
            sendOverride({
              width: w, height: h,
              fps: fpsStillValid ? lastOverride.fps : 0,
              fpsNumerator: fpsStillValid ? lastOverride.fpsNumerator : 0,
              fpsDenominator: fpsStillValid ? lastOverride.fpsDenominator : 1,
              format: fmtStillValid ? lastOverride.format : '',
            });
          }
        );

        const fpsValues = uniqueFps();
        makeSelectRow('source.framerate',
          fpsValues.map(f => ({ value: rateKey(f), label: rateLabel(f) })),
          overrideRateKey() === '0/1' ? '' : overrideRateKey(),
          (newVal) => {
            const selected = newVal
              ? lastAvailableFormats.find(f =>
                  f.width === lastOverride.width &&
                  f.height === lastOverride.height &&
                  RENDERABLE.has(f.format) && rateKey(f) === newVal)
              : null;
            const newFps = selected ? selected.fps : 0;
            const newFpsNumerator = selected
              ? Number(selected.fpsNumerator || selected.fps) : 0;
            const newFpsDenominator = selected
              ? Number(selected.fpsDenominator || 1) : 1;
            // Stale-child invalidation: if the persisted format is no
            // longer renderable at (currentResolution, newFps), reset
            // format to Auto. Prevents sending {res, fps, format}
            // where (res, fps, format) is not in availableFormats.
            let format = lastOverride.format;
            if (format) {
              const validAtCombo = lastAvailableFormats.filter(f =>
                RENDERABLE.has(f.format) &&
                f.width === lastOverride.width &&
                f.height === lastOverride.height &&
                (!newVal || rateKey(f) === newVal));
              if (!validAtCombo.some(f => f.format === format)) format = '';
            }
            sendOverride({
              width: lastOverride.width,
              height: lastOverride.height,
              fps: newFps,
              fpsNumerator: newFpsNumerator,
              fpsDenominator: newFpsDenominator,
              format: format,
            });
          }
        );

        const formatValues = uniqueFormats();
        makeSelectRow('source.format',
          formatValues.map(f => ({ value: f, label: f })),
          lastOverride.format,
          (newVal) => {
            sendOverride({
              width: lastOverride.width,
              height: lastOverride.height,
              fps: lastOverride.fps,
              fpsNumerator: lastOverride.fpsNumerator,
              fpsDenominator: lastOverride.fpsDenominator,
              format: newVal,
            });
          }
        );

        const divider = document.createElement('div');
        divider.className = 'source-popover-divider';
        pop.appendChild(divider);

        const refresh = document.createElement('div');
        refresh.className = 'source-popover-action';
        refresh.textContent = t('source.refresh');
        refresh.addEventListener('click', ev => {
          ev.stopPropagation();
          post('refreshDevices');
        });
        pop.appendChild(refresh);
        refreshSourceSelects();
      };

      const openPop = () => {
        renderPopover();
        pop.dataset.open = 'true';
        // Ask the backend for a fresh enumeration so the list reflects
        // anything plugged in since the last state push. The result
        // arrives asynchronously and re-renders the popover in place.
        post('getDeviceList');
      };
      const togglePop = () => {
        (pop.dataset.open === 'true') ? closePop() : openPop();
      };

      trigger.addEventListener('click', e => {
        e.stopPropagation();
        togglePop();
      });
      // The original popover is inside its trigger. Input/editor clicks must
      // not bubble into that trigger and toggle the containing source menu.
      pop.addEventListener('click', e => e.stopPropagation());
      document.addEventListener('click', e => {
        if (!pop.contains(e.target) && !trigger.contains(e.target)) closePop();
      });
      document.addEventListener('keydown', e => {
        if (e.key === 'Escape') closePop();
      });
      pop.addEventListener('focusout', () => {
        queueMicrotask(() => {
          if (pop.dataset.open === 'true') refreshSourceSelects();
        });
      });

      // Public update entry point. applyState pushes the full set on
      // every state refresh; the picker re-renders the popover in
      // place if it's currently open.
      //
      // Re-render is suppressed when focus is inside the popover.
      // PushSettingsState fires every ~1s on the C++ side while the
      // settings menu is visible. Without this guard, the periodic
      // re-render replaces the capture-format children right when a user
      // has the native <select>
      // dropdown open, which closes their dropdown mid-pick. The next
      // un-focused state push will catch the latest data.
      window._updateSource = (devices, active, availableFormats, override) => {
        lastDevices = Array.isArray(devices) ? devices : [];
        lastActive = typeof active === 'string' ? active : '';
        lastAvailableFormats = Array.isArray(availableFormats)
          ? availableFormats : [];
        const incoming = override || { width: 0, height: 0, fps: 0, format: '' };
        // Preserve an explicit zero numerator. Integer-only legacy
        // preferences intentionally use { fps: 59, fpsNumerator: 0 } so the
        // capture negotiator can resolve that display bucket against the
        // card's native rational (for example 60000/1001). Reconstructing
        // 59/1 here would turn an unresolved legacy preference back into the
        // exact-rate bug when the user later changes another Source field.
        const hasIncomingNumerator = Object.prototype.hasOwnProperty.call(
          incoming, 'fpsNumerator');
        const hasIncomingDenominator = Object.prototype.hasOwnProperty.call(
          incoming, 'fpsDenominator');
        lastOverride = {
          width: Number(incoming.width) || 0,
          height: Number(incoming.height) || 0,
          fps: Number(incoming.fps) || 0,
          fpsNumerator: hasIncomingNumerator
            ? (Number(incoming.fpsNumerator) || 0)
            : (Number(incoming.fps) || 0),
          fpsDenominator: hasIncomingDenominator
            ? (Number(incoming.fpsDenominator) || 1)
            : 1,
          format: typeof incoming.format === 'string' ? incoming.format : ''
        };
        nameEl.textContent = lastActive || t('meta.none');
        trigger.dataset.multi = (lastDevices.length > 1) ? 'true' : 'false';
        if (pop.dataset.open === 'true') {
          const interacting = document.activeElement
            && pop.contains(document.activeElement);
          if (!interacting) renderPopover();
          else refreshSourceSelects();
        }
      };
    })();

    // HDMI identity uses its own picker and never requests device enumeration
    // or capture-format changes. Selection is confirmed by the native state.
    (() => {
      const picker = document.getElementById('hdmi-source-picker');
      const trigger = document.getElementById('hdmi-source-trigger');
      const nameEl = document.getElementById('meta-hdmi-source');
      const customRow = document.getElementById('hdmi-source-custom-row');
      const edit = document.getElementById('hdmi-source-custom-edit');
      const editor = document.getElementById('hdmi-source-custom-editor');
      const input = document.getElementById('hdmi-source-custom-input');
      const error = document.getElementById('hdmi-source-custom-error');
      let choices = [];
      let options = [];
      let selected = 'auto';
      let effective = '';
      let custom = '';
      let choicesKey = '';
      const choiceLabel = choice => choice.value === 'auto' ? t('value.auto')
        : choice.value === 'other' ? t('hdmiSource.otherOption') : choice.label;
      const closeEditor = (restoreFocus = false) => {
        editor.hidden = true;
        if (restoreFocus) trigger.focus();
      };
      const renderOptions = () => {
        trigger.replaceChildren();
        options = choices.map(choice => {
          const { value } = choice;
          const option = document.createElement('option');
          option.value = value;
          trigger.appendChild(option);
          return { option, choice };
        });
      };
      const openEditor = () => {
        editor.hidden = false;
        error.hidden = true;
        input.value = custom;
        input.focus();
        input.select();
      };
      // Use the same HTML select and shared CSS as resolution/FPS/format.
      trigger.addEventListener('click', event => event.stopPropagation());
      trigger.addEventListener('change', event => {
        event.stopPropagation();
        const value = trigger.value;
        if (!choices.some(choice => choice.value === value)) {
          trigger.value = selected;
          return;
        }
        post('setManualHdmiSource', value);
        if (value === 'other') openEditor();
        else closeEditor(true);
      });
      edit.addEventListener('click', openEditor);
      picker.addEventListener('keydown', event => {
        if (event.key === 'Escape' && !editor.hidden) {
          event.preventDefault();
          event.stopPropagation();
          closeEditor(true);
        }
      });
      document.getElementById('source-trigger').addEventListener('click', () => closeEditor());
      document.addEventListener('click', event => {
        if (!picker.contains(event.target)) closeEditor();
      });
      const saveCustom = () => {
        const name = input.value.trim();
        if (/[\x00-\x1f\x7f-\x9f\u2028\u2029]/u.test(input.value) ||
            Array.from(name).length > 64) {
          error.hidden = false;
          input.focus();
          return;
        }
        post('setManualHdmiSourceCustom', name);
        closeEditor(true);
      };
      document.getElementById('hdmi-source-custom-save').addEventListener('click', saveCustom);
      document.getElementById('hdmi-source-custom-cancel').addEventListener('click', () => closeEditor(true));
      input.addEventListener('keydown', event => {
        if (event.key === 'Enter') {
          event.preventDefault();
          saveCustom();
        }
      });
      window._updateManualHdmiSource = (manual, sourceLabel, customName, nativeChoices) => {
        // Fixed labels come from the same native mapping as title and Discord.
        if (Array.isArray(nativeChoices)) {
          const valid = nativeChoices.filter(choice => choice &&
            typeof choice.value === 'string' && typeof choice.label === 'string');
          const key = JSON.stringify(valid);
          if (key !== choicesKey) {
            choicesKey = key;
            choices = valid;
            renderOptions();
          }
        }
        trigger.disabled = options.length === 0;
        if (typeof manual === 'string')
          selected = choices.some(choice => choice.value === manual) ? manual : 'auto';
        if (typeof sourceLabel === 'string') effective = sourceLabel;
        if (typeof customName === 'string') custom = customName;
        options.forEach(({ option, choice }) => {
          const label = choiceLabel(choice);
          if (option.textContent !== label) option.textContent = label;
        });
        trigger.value = selected;
        customRow.hidden = selected !== 'other';
        nameEl.textContent = selected === 'auto' ? t('value.auto')
          : selected === 'other' && !custom ? t('hdmiSource.other')
          : effective;
        trigger.title = effective
          ? `${nameEl.textContent} · ${effective}`
          : t('meta.hdmiSourceInfo');
      };
      window._updateManualHdmiSource('auto', '');
    })();

    // Transient toast notification. Pushed from C++ via state.notification
    // (single-use, cleared on the C++ side so the same notice never
    // re-fires after dismissal).
    const ensureToast = () => {
      let toast = document.getElementById('toast');
      if (!toast) {
        toast = document.createElement('div');
        toast.id = 'toast';
        toast.className = 'toast';
        document.body.appendChild(toast);
      }
      return toast;
    };

    const showToast = (text) => {
      const toast = ensureToast();
      toast.textContent = text;
      toast.classList.add('toast-show');
      clearTimeout(toast._toastTimer);
      toast._toastTimer = setTimeout(() => {
        toast.classList.remove('toast-show');
      }, 3000);
    };

    // Rich toast for "screenshot saved" events. Shows filename plus a
    // clickable link that posts openScreenshotFolder back to C++, which
    // opens Windows Explorer with the file pre-selected. Auto-dismisses
    // after 3500ms (slightly longer than the plain toast so users have
    // time to read and click).
    const showScreenshotToast = (filepath) => {
      const toast = ensureToast();
      const lastSlash = Math.max(filepath.lastIndexOf('\\'),
                                 filepath.lastIndexOf('/'));
      const filename = lastSlash >= 0
        ? filepath.substring(lastSlash + 1)
        : filepath;
      toast.textContent = '';
      const prefix = document.createElement('span');
      prefix.textContent = t('toast.screenshotSaved');
      const link = document.createElement('span');
      link.textContent = filename;
      link.style.textDecoration = 'underline';
      link.style.cursor = 'pointer';
      link.style.color = 'var(--accent)';
      link.title = t('toast.openFolder');
      link.addEventListener('click', () => {
        post('openScreenshotFolder');
      });
      toast.appendChild(prefix);
      toast.appendChild(link);
      toast.classList.add('toast-show');
      clearTimeout(toast._toastTimer);
      toast._toastTimer = setTimeout(() => {
        toast.classList.remove('toast-show');
      }, 3500);
    };

    const applyState = s => {
      if (!s) return;
      if (s.locale) applyTranslations(s.locale);
      if (languageSelect && s.languagePreference) languageSelect.value = s.languagePreference;
      const setToggle = (id, on) => { const el = document.getElementById(id); if (el) el.classList.toggle('on', !!on); };
      const setText = (id, txt) => { const el = document.getElementById(id); if (el) el.textContent = txt; };
      const setHidden = (id, hidden) => { const el = document.getElementById(id); if (el) el.hidden = hidden; };
      setToggle('toggle-hdr', s.hdrEnabled);
      setToggle('toggle-color', s.colorExpansion);
      setHidden('color-expansion-advanced', s.colorExpansionAvailable !== true);
      setToggle('toggle-nis', s.nisEnabled);
      setToggle('toggle-vsync', s.vsync);
      setToggle('toggle-low-latency', s.lowLatency);
      setToggle('toggle-prevent-sleep', s.preventSleep);
      if (s.presentPacing) {
        setText('pacing-val',
          s.presentPacing === 'unique'   ? t('value.sourceFrameRate') :
          s.presentPacing === 'captured' ? t('value.captureRate')      : t('value.displayRefresh'));
      }
      setToggle('toggle-mute', s.audioMuted);

      if (typeof s.volume === 'number') {
        const slider = document.getElementById('slider-volume');
        if (slider && slider._setFromCpp) slider._setFromCpp(s.volume * 100);
      }
      setPiPOpacityVisual(s.pipOpacity);
      if (s.aspectRatio) setText('aspect-val', localizeValue(s.aspectRatio));
      if (s.panelSide) {
        setText('panel-side-val', localizeValue(s.panelSide));
        document.documentElement.dataset.side = String(s.panelSide).toLowerCase();
      }
      let configWarning = document.getElementById('config-warning');
      if (!configWarning) {
        configWarning = document.createElement('div');
        configWarning.id = 'config-warning';
        configWarning.setAttribute('role', 'status');
        configWarning.style.cssText = 'flex:none;padding:12px 20px;color:var(--warn);white-space:normal;font-size:13px;line-height:1.5';
        document.querySelector('.content').prepend(configWarning);
      }
      configWarning.textContent = typeof s.configWarning === 'string' ? s.configWarning : '';
      configWarning.hidden = !configWarning.textContent;
      const noSignalMode = s.noSignalMode === 'image' ? 'image' : 'default';
      setText('no-signal-mode-val', t(
        noSignalMode === 'image'
          ? (s.noSignalImage && s.noSignalImageAvailable === false ? 'value.customImageUnavailable' : 'value.customImage')
          : 'value.nitlinkDefault'));
      const noSignalFit = ['contain', 'cover', 'stretch'].includes(s.noSignalFit)
        ? s.noSignalFit : 'contain';
      setText('no-signal-fit-val', t(`value.${noSignalFit}`));
      setText('no-signal-dim-val', s.noSignalDimImage === false
        ? t('value.off') : t('value.on'));
      const customNoSignal = noSignalMode === 'image';
      const path = typeof s.noSignalImage === 'string' ? s.noSignalImage : '';
      setHidden('no-signal-image-row', !customNoSignal);
      setHidden('no-signal-fit-row', !customNoSignal);
      setHidden('no-signal-dim-row', !customNoSignal);
      setHidden('no-signal-remove', !customNoSignal || !path);
      const imageValue = document.getElementById('no-signal-image-value');
      if (imageValue) {
        const slash = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
        imageValue.textContent = path ? path.substring(slash + 1) : t('meta.none');
        imageValue.title = path || t('meta.none');
      }
      const hasSignal = s.negotiatedWidth > 0 && s.negotiatedHeight > 0 && s.negotiatedFps > 0;
      setText('meta-resolution', hasSignal
        ? `${s.negotiatedWidth}x${s.negotiatedHeight} / ${s.negotiatedFps} ${t('units.fps')}`
        : t('meta.none'));
      setText('meta-format', s.negotiatedFormat === 'Unknown' ? t('value.auto') : s.negotiatedFormat);
      setText('meta-link', s.linkText);
      setText('meta-latency', s.latencyText);

      if (window._updateSource) {
        window._updateSource(
          s.captureDevices,
          s.activeDevice,
          s.availableFormats,
          s.captureFormatOverride
        );
      }
      window._updateManualHdmiSource(
        s.manualHdmiSource, s.effectiveHdmiSource, s.manualHdmiSourceCustom, s.manualHdmiSourceOptions);

      // One-shot user-facing notice from the C++ pipeline. Most state
      // pushes carry an empty string here; non-empty means the C++ side
      // wants the user to see something (currently: format-override
      // fallback). The C++ side already cleared its internal flag when
      // it produced this string, so re-runs of applyState do not
      // re-show stale toasts.
      if (typeof s.notification === 'string' && s.notification.length > 0) {
        showToast(s.notification);
      }

      // Screenshot saved: rich toast with clickable filename that opens
      // the containing folder in Explorer via openScreenshotFolder.
      if (typeof s.screenshotSaved === 'string' && s.screenshotSaved.length > 0) {
        showScreenshotToast(s.screenshotSaved);
      }

      // HDR auto-detect notice: shown when the card cannot read HDR
      // source state (generic Media Foundation devices, for example).
      // Tells the user to toggle HDR manually with Alt+H.
      const hdrNotice = document.getElementById('hdr-autodetect-notice');
      if (hdrNotice) {
        if (s.hdrAutoDetectSupported === false) {
          hdrNotice.removeAttribute('hidden');
        } else {
          hdrNotice.setAttribute('hidden', '');
        }
      }
    };

    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.addEventListener('message', evt => {
        let msg = evt.data;
        if (typeof msg === 'string') try { msg = JSON.parse(msg); } catch { return; }
        if (msg && msg.state) applyState(msg.state);
        if (msg && typeof msg.toast === 'string') showToast(msg.toast);
        if (msg && typeof msg.pipOpacity === 'number') setPiPOpacityVisual(msg.pipOpacity);
      });
      post('ready');
    }
