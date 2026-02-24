# 3DS Activity Sync — Feature Milestones

This document describes planned features as milestones. Each milestone defines **what** the feature should do from the user’s perspective, without implementation details.

---

## M1 — Sort options for the main list

**Goal:** Let the user change how the game list is ordered.

**Behavior:**

- The main game list supports multiple sort modes. The user can cycle through them (e.g. via a key or menu).
- **Sort by total playtime** — Descending (most played first).
- **Sort by launch count** — Descending (most launches first).
- **Sort by session count** — Descending (most sessions first).
- **Sort by last played** — Descending (most recently played first). THIS SHOULD BE THE DEFAULT
- **Sort by first played** — User choice: ascending (oldest first) or descending (newest first).
- **Sort by name** — Alphabetical A–Z.
- The current sort mode is visible to the user (e.g. in the header or status bar: “Sort: Playtime”, “Sort: Launches”, etc.).

---

## M2 — Rankings screen

**Goal:** A dedicated view that shows “top N” games by different metrics.

**Behavior:**

- The user can open a **Rankings** screen (e.g. from a shoulder key).
- **Top N by playtime** — List of games with the highest total playtime. Optionally show a horizontal bar for each entry (length proportional to playtime vs. the top game).
- **Top N by launches** — Same idea, ordered by launch count.
- **Top N by sessions** — Same idea, ordered by session count.
- **Most recent** — Games ordered by last played date (most recently played first).
- The value of N is fixed or configurable so the list fits the screen (e.g. top 10–15). User can scroll if more entries are shown.
- From this screen, the user can return to the main list (e.g. B or START). Optionally, selecting a game opens the existing per-title detail view.

---

## M3 — Pie chart (playtime share)

**Goal:** A visual breakdown of playtime by game.

**Behavior:**

- The user can open a **Pie chart** view (e.g. from the menu or a dedicated entry).
- The chart shows how total playtime is split across games.
- Only the **top K games** (e.g. 10–12) are given distinct slices; the rest are grouped into an **“Other”** slice so the chart stays readable.
- Each slice is clearly associated with a game (or “Other”) via a legend or labels (e.g. on the bottom screen or around the chart).
- Colors are distinct and consistent (e.g. reuse the app’s existing color palette). The “Other” slice has its own color.
- The user can return to the previous screen (e.g. B or START).

---

## M4 — Bar chart (playtime)

**Goal:** A simple bar chart of playtime for top games.

**Behavior:**

- The user can open a **Bar chart** view.
- Horizontal bars represent playtime for each of the **top N games** (e.g. 8–10). Bar length is proportional to total playtime (e.g. longest bar = top game).
- Each bar is labeled (game name or short name) so the user can tell which game is which.
- The chart fits on one screen (top or bottom); if needed, the list is scrollable.
- The user can exit the view and return to the main list or menu.

---

## M5 — Time-based stats (recent playtime)

**Goal:** Show playtime for a recent time window.

**Behavior:**

- **Playtime in last 7 days** — For each title (or in aggregate), show total playtime where the session timestamp falls within the last 7 days.
- **Playtime in last 30 days** — Same for the last 30 days.
- These can be shown in a dedicated “Recent” section, on the bottom stats screen, in the Rankings screen, or in the per-title detail view. Exact placement is flexible; the milestone is that the user can see recent playtime for a defined window (7 and/or 30 days).

---

## M6 — Activity over time (chart)

**Goal:** Show how playtime is distributed over calendar time.

**Behavior:**

- The user can open an **Activity over time** view.
- Playtime is grouped by **day** or **week** (configurable or fixed).
- A simple chart (e.g. bar strip or line) shows playtime per day or per week over a chosen range (e.g. last 30 days, last 12 weeks).
- The chart makes it easy to see busy vs. quiet periods. No need for complex interaction; scrolling or a fixed window is enough.
- The user can exit and return to the main flow.

---

## M7 — “Recently played” filter

**Goal:** Filter the main list to games played in a recent time window.

**Behavior:**

- The user can enable a filter such as **“Last 30 days”** (or similar: 7 days, 14 days).
- When enabled, the main game list shows only titles that have at least one session in that window (based on session timestamps).
- When disabled, the list shows all titles as today. The filter state is clear in the UI (e.g. status bar: “Filter: Last 30 days” or “Filter: All time”).

---

## M8 — Average session length (detail and ranking)

**Goal:** Surface “average time per session” (or per launch) for each game.

**Behavior:**

- **In per-title detail view** — Show **average session length** (e.g. total playtime ÷ session count, or ÷ launch count). Format as time (e.g. “12m” or “1h 5m”).
- **Optional ranking** — User can view a list or ranking of games by “longest average session” (games you tend to play in longer sittings). This can be a sort option (M1) or a dedicated Rankings view (M2).

---

## M9 — Streaks / consecutive days

**Goal:** Show “played X days in a row” for a title.

**Behavior:**

- Using the session log, compute **consecutive days played** for each title (e.g. maximum streak of calendar days with at least one session).
- **In per-title detail view** — Display something like “Longest streak: X days” (or “Played X days in a row”) when meaningful.
- No need for real-time “current streak” unless desired; “longest streak” is enough for this milestone.

---

## M10 — Export to SD

**Goal:** Export activity data to a file on the SD card for use on PC or elsewhere.

**Behavior:**

- The user can trigger **Export** (e.g. from the START menu).
- The app writes a **text file** (e.g. `export.txt` or `activity_export_YYYYMMDD.txt`) to a known folder on SD (e.g. `sdmc:/3ds/3ds-activity-sync/`).
- The file contains one line per title (or similar structure) with at least: **name**, **total playtime**, **launch count**, **sessions**, **first played**, **last played**. Format can be human-readable or CSV.
- After export, the user receives clear feedback (e.g. “Exported to …/export.txt” or “Export failed”).
- No implementation detail is mandated; the milestone is “user can get a file on SD with the described data.”

---

## M11 — Favorites / hidden list

**Goal:** Let the user mark titles as favorites or hidden to customize the list.

**Behavior:**

- **Favorites** — User can mark titles as “favorite.” Favorites can be sorted to the top of the main list when a “Show favorites first” option is on, or shown in a dedicated “Favorites” view.
- **Hidden** — User can mark titles as “hidden.” Hidden titles are excluded from the main list (and optionally from rankings/charts) so the list is less cluttered (e.g. system or test titles).
- Favorites and hidden state are **persisted** (e.g. on SD) so they survive app restart and are per-device unless sync is extended later.
- The user can change favorite/hidden status from the detail view or from the list (e.g. a key or button). The UI clearly indicates which titles are favorite or hidden when relevant.

---

## M12 — Search / filter by name

**Goal:** Filter the game list by text in the title name.

**Behavior:**

- The user can open a **search** or **filter** mode (e.g. from menu or a key).
- **If on-screen keyboard is available** — User types a string; the main list shows only titles whose name contains that string (case-insensitive or case-sensitive; one or the other is consistent).
- **Simpler variant** — Preset filters (e.g. “Nintendo”, “indie”) if titles can be categorized (e.g. by title ID ranges or a small config). User selects a preset and the list is filtered accordingly.
- When search is active, the UI shows that a filter is on (e.g. “Filter: …” or “Search: …”) and how to clear it. Clearing restores the full list (with current sort).

---

## M13 — Expanded bottom-screen stats

**Goal:** Show more aggregate stats on the bottom screen without changing the layout philosophy.

**Behavior:**

- In addition to **Games tracked**, **Total playtime**, and **Syncs**, the bottom screen shows one or more of:
  - **Total time played** — Total playtime that was recorded.
  - **Most played game** — Name (or short name) of the title with the highest total playtime, plus its playtime.
  - **Total launches** — Sum of launch_count across all shown titles.
  - **Total sessions** — Sum of session count across all shown titles.
- Layout remains readable; stats can be abbreviated (e.g. “Most: Game Name — 45h”) or split across lines. The milestone is “user sees these additional stats on the bottom screen.”
  OPTIONALLY:
  - **Total time played on this system** — Total playtime that was recorded on this device only (before or without merging from other systems).
  - **Total time played on all synced systems** — Total playtime in the merged dataset (combined across all devices that have been synced).


---

## Summary table

| Milestone | Feature |
|-----------|---------|
| M1 | Sort options for the main list |
| M2 | Rankings screen |
| M3 | Pie chart (playtime share) |
| M4 | Bar chart (playtime) |
| M5 | Time-based stats (recent playtime) |
| M6 | Activity over time (chart) |
| M7 | “Recently played” filter |
| M8 | Average session length (detail and ranking) |
| M9 | Streaks / consecutive days |
| M10 | Export to SD |
| M11 | Favorites / hidden list |
| M12 | Search / filter by name |
| M13 | Expanded bottom-screen stats |
