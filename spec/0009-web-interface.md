# 0009 — PWA Web Interface with Day/Night/Auto Theme Support

- **Status:** Draft
- **Author(s):** Antigravity
- **Created:** 2026-06-18
- **Last updated:** 2026-06-18
- **Related specs:** `0006-device-provisioning.md`, `0008-config-and-network-lifecycle.md`

## 1. Summary

This specification defines the Progressive Web App (PWA) web interface for SomnoTrace, which serves both the SoftAP configuration portal and the runtime status interface in station (STA) mode. The interface loads as a single page, fetches data dynamically via `/api` endpoints, and implements day, night, and auto theme configurations with user persistence.

## 2. Motivation / goals

- Provide a premium, responsive, and modern user experience (UX) without using absolute white or absolute black backgrounds.
- Enable offline-readiness and a native-app feel by implementing Progressive Web App (PWA) standards (service worker, manifest).
- Support automated or manual light/dark themes to fit bedroom environments (reducing blue light / bright light at night).

## 3. Non-goals

- Out of scope: The implementation of back-end `/api` endpoints that aren't required for Wi-Fi provisioning.
- Out of scope: Local storage of EDF files inside the PWA database.

## 4. Behaviour

### 4.1 Progressive Web App (PWA) Architecture
- **Single Page Application:** The interface must load once and update dynamic fields using vanilla JavaScript and `fetch` from `/api` endpoints.
- **PWA Manifest:** A `manifest.json` file must define the application name, startup URL, colors, and icons.
- **Service Worker:** A basic service worker must be registered to cache shell assets (`/`, `/manifest.json`, styles, scripts) for offline capability.

### 4.2 Theme Management
- **Themes:**
  - **Day Mode:** A modern, clean light palette (e.g. background `#f4f6f9`, card background `#ffffff`, text `#1e293b`).
  - **Night Mode:** A premium, eye-friendly dark palette (e.g. background `#0f172a`, card background `#1e293b`, text `#f8fafc`).
- **Auto Mode:** Automatically toggles Day Mode between 08:00 AM and 08:00 PM (20:00) local time, and Night Mode outside those hours.
- **Persistence:** Selected theme mode (day, night, auto) is persisted in browser `localStorage`. On page load, the saved setting is immediately applied before rendering to avoid light flashing.

### 4.3 Endpoints
- `/`: Serves the unified index.html file containing the HTML structure, CSS styles, and client JavaScript.
- `/wifi`: Serves the same unified index.html, pre-routing the client SPA directly into the Wi-Fi setup view.
- `/manifest.json`: Web app manifest for installation.
- `/api/scan`: (SoftAP mode) Returns a list of nearby Wi-Fi access points.
- `/api/status`: Returns the current system/connection status and the list of currently configured SSIDs (excluding passwords) in NVS.

### 4.4 Responsive Layout
- **Cross-Platform Compatibility:** The layout must adapt dynamically to small mobile screens, tablets, and large desktop monitors.
- **Centering and Boundaries:** The interface is contained in a viewport-centered card with a fluid width up to a maximum boundary (`400px`) to remain clean and focused on desktop while filling the width on mobile.
- **Fluid Padding and Input Fields:** Interactive blocks (dropdowns, inputs, buttons) scale to occupy 100% of the card width, facilitating touch targets on mobile and structured presentation on desktop.

### 4.5 Wi-Fi Setup and Profiles
- **Multi-Profile Configuration:** The setup page allows the user to add and save up to 4 distinct Wi-Fi network credentials. By default, one profile input block is shown, with a button to dynamically add more blocks up to the maximum limit of 4.
- **Client-Side BSSID De-duplication:** During Wi-Fi network scanning, if multiple access points share the same SSID, the client-side JavaScript filters the list to retain only the single entry with the strongest signal strength (RSSI), presenting a clean, de-duplicated list sorted by RSSI in descending order.
- **Manual Input Fallback:** For hidden or unbroadcasted networks, each profile block features a toggle switch to swap between selecting a scanned SSID from the dropdown and typing a custom SSID manually.
- **Password Visibility Toggle:** A password reveal icon is included on each password input field. Clicking it toggles the visibility state between masked (`password`) and readable plain text (`text`).
- **NVS Profile Prefilling:** On page initialization, the app fetches `/api/status` to retrieve the current list of saved SSIDs. The setup view dynamically creates the corresponding number of blocks and pre-fills them as manual text inputs.
- **Connected Mode Setup Access:** In connected (STA) mode, the setup interface is accessible directly via the `/wifi` path, or via a "Configure Wi-Fi / Add Networks" button on the main connected screen. A "Back to Status" button is shown to allow the user to return to the status screen without restarting the device.

## 5. Acceptance criteria

- [ ] Web pages load as a single progressive web app (PWA) with manifest and service worker registration.
- [ ] UI features a theme switch allowing selection between "Day", "Night", and "Auto".
- [ ] Theme selection persists across browser reloads.
- [ ] In Auto mode, the theme switches automatically based on local time (Day: 8 AM - 8 PM; Night: 8 PM - 8 AM).
- [ ] Absolute white (`#ffffff` as body bg) and absolute black (`#000000` as body bg) are avoided in the color palette.
- [ ] Interface works on both mobile and desktop screens, automatically scaling and centering components.
- [ ] Support up to 4 Wi-Fi profile slots in the setup interface.
- [ ] Multiple BSSIDs with the same SSID are de-duplicated, showing only the strongest signal.
- [ ] Users can toggle between scanned network dropdown and manual SSID text input.
- [ ] Password reveal button toggles text/masked display.
- [ ] Previously saved profiles are retrieved from NVS and populated on load (without showing passwords).
- [ ] Setup view is accessible in connected mode via `/wifi` or a button, and allows going back to the status view.

## 6. Security / privacy considerations

- Wi-Fi credentials entered via the portal must be transmitted securely over POST requests.

## 7. Open questions

None.

## 8. Changelog

- 2026-06-18: Initial draft.
- 2026-06-18: Added mobile/desktop responsive layout specifications.
- 2026-06-18: Added multi-profile Wi-Fi configuration and advanced input options.
- 2026-06-18: Documented prefilling of saved SSIDs from NVS and `/wifi` STA mode routing.
