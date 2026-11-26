#pragma once
// Widgets of the Violet menu (apex_gui.cpp and the features' own controls): cards, the iOS-style toggle switch, rows
// (switch, slider, segmented, custom controls), the collapsed "Advanced" section, notes, buttons, status pills, sidebar
// items and icon buttons. Icons are Lucide vector strokes (ui/icons.h). Sizes are designed at 1080p with text size 1 and
// multiplied by Unit(), so they follow the text (resolution x the user's text size). Colours go through
// ImGui::GetColorU32(ImVec4), which applies the disabled alpha inside BeginDisabled (a disabled row dims as a whole).
//
// Design tokens (docs/ui.md "Design tokens"):
//  - spacing scale kSpace1..4 = 4 / 8 / 12 / 16 (x Unit());
//  - type scale: page title 1.3x bold, card title 1.05x bold, row label 1x, description / value / end labels 0.87x muted,
//    group label regular 1x muted, without artificial tracking;
//  - rows: consecutive rows (switch, slider, segmented, control, overview) are kRowGap apart with a hairline divider in the
//    middle; any other element (note, button, "Advanced", group label) keeps kSpace2 from a row above it.
//  - rows with a default (the optional default argument) show a small violet dot after the label while their value differs
//    from it, and on hover a small Reset button (rotate-ccw) that puts that one setting back;
//  - SetNextRowBadge adds an amber chip after the next row's label ("Reload save").
//
// The menu's search (docs/ui.md "Search"): BeginFilter / EndFilter put the widgets in filter mode, where rows draw only
// when they match the query (each after a small breadcrumb link) and every other element (card frames, headers without
// a matching switch, notes, buttons, group labels, "Advanced" rows) draws nothing.
//
// Undo (docs/ui.md "Undo"): rows, card-header switches and overview switches report what the user changed
// (ReportChange); the menu turns the last report of a frame into the undo toast.
#include "imgui.h"
#include "icons.h"
#include "violet_theme.h"
#include <climits>
#include <limits>
#include <string>

namespace ApexUi {

// 1 at 1080p with text size 1 (the overlay's style.FontScaleMain)
float Unit();

// ---- optional defaults of rows (changed dot + per-row Reset) ----
// "No default": the row shows no dot and no Reset button
inline constexpr float kNoDefault = std::numeric_limits<float>::quiet_NaN();
inline constexpr int kNoDefaultIndex = INT_MIN;
// A switch's default: implicitly made from a bool; {} = none
struct BoolDefault {
    signed char value = -1; // -1 none, 0 off, 1 on
    BoolDefault() = default;
    BoolDefault(bool on) : value(on ? 1 : 0) {}
};

// The next row (SwitchRow, Slider, SliderPercent, SegmentedRow, BeginControlRow) gets a small chip after its label, with
// a tooltip. Consumed by that row. Amber for "Reload save"; the Presets page uses success green for "Applied".
void SetNextRowBadge(const char* text, const char* tooltip, unsigned rgb = 0xE0A84F);
// The next row shows the changed dot with this tooltip, without a Reset button (a preset changed since it was applied)
void SetNextRowChangedDot(const char* tooltip);
// The next row's label is user data (a profile name): shown and searched as it is, never translated
void SetNextRowUntranslated();
// The next row's description in amber: something about that row needs the player (a doubled effect, a missing file)
void SetNextRowWarning();
// The next BeginControlRow's leading icon in another colour than the accent (a muted drag grip)
void SetNextRowIconColor(unsigned rgb);
// The next SwitchRow at least this tall (pixels): a switch among slider rows keeps their rhythm (a shader's values)
void SetNextRowMinHeight(float px);
inline constexpr float kRowIconBox = 22.0f; // the leading icon's box of a control row (units)

// ---- search filter (the menu's search results) ----
// While a filter is on, the rows draw only when every word of the query (case-insensitive) appears in their label or
// description; each matching row gets a small muted breadcrumb link above it (the current crumb). Card frames, notes,
// buttons, group labels, page titles, tab bars and "Advanced" rows draw nothing; a card header with a switch draws as a
// switch row when its title or subtitle matches. Render thread only; not nestable.
void BeginFilter(const char* query);
// The breadcrumb ("Lighting › Lamps") shown above the following matching rows; id is returned by EndFilter when clicked
void SetFilterCrumb(const char* crumb, int id);
// Ends filter mode; returns the number of rows drawn. *clickedCrumb = the id of a clicked breadcrumb, else -1.
int EndFilter(int* clickedCrumb);
bool FilterActive();
// Every word of query appears in text (case-insensitive); an empty query matches everything
bool MatchesQuery(const char* query, const char* text);

// ---- change reports (undo toast) ----
// What the user just changed, in words ("Night Lights turned on", "Contrast changed"); the last one of a frame wins.
// Row widgets report by themselves; call it for other changes (Reset buttons, "Turn on" buttons).
// text is English (a literal from the table): the toast shows it translated, the log gets it as given
// group (optional): changes of one kind made one after another (removals). While the toast of that group still shows,
// a report of the same group keeps the toast's Undo (the state before the group's first change) and replaces its text.
void ReportChange(const char* text, const char* group = nullptr);
// True while the undo toast shows a change of this group: the caller keeps adding to it (see ReportChange)
bool ChangeGroupOpen(const char* group);
// The menu sets the group of the toast it shows each frame ("" when none)
void SetOpenChangeGroup(const std::string& group);
// Off: reports are ignored (the Developer page: its switches are not part of the undoable state)
void SetChangeReporting(bool on);
// The report of this frame (and clears it); false when nothing was reported
// english (optional): the same report in English, for the log
bool TakeChange(std::string& text, std::string* english = nullptr, std::string* group = nullptr);

// ---- slider drag state (the menu fades while a slider is dragged) ----
// A slider was active in the previous frame
bool SliderDragging();
// While on, the active slider's row draws at full opacity (the rest of the menu is faded by the caller)
void SetKeepActiveSliderOpaque(bool on);

// Spacing scale (x Unit())
inline constexpr float kSpace1 = 4.0f;
inline constexpr float kSpace2 = 8.0f;
inline constexpr float kSpace3 = 12.0f;
inline constexpr float kSpace4 = 16.0f;
inline constexpr float kRowGap = kSpace4; // label-to-label gap between two rows (the divider sits in the middle)

// Type scale (x the base font size)
inline constexpr float kPageTitleScale = 1.3f;
inline constexpr float kCardTitleScale = 1.05f;
inline constexpr float kSmallScale = 0.87f; // descriptions, values, end labels, pills
inline constexpr float kGroupScale = 0.8f;  // group labels inside a card ("WALLS")

// Icon sizes (x Unit())
inline constexpr float kIconSmall = 14.0f;  // inline with text: notes, buttons, pills
inline constexpr float kIconMedium = 18.0f; // card headers, sidebar, overview rows

// ---- text ----
// Wrapped tooltip on the last item (also when it is disabled)
void Tooltip(const char* text);
// Large bold page title and its muted subtitle, then the gap before the first card
void PageTitle(const char* title, const char* subtitle);
// Every dialog in one shape: kModalNarrow (one field or a question) or kModalWide (a list or a grid) units wide (never
// wider than the screen less 32 px), centred on the screen, a card's padding (kSpace4) and corners, its height following
// its content. Then CardHeader + CardDivider, the body, and CardDivider + the buttons on the right (Cancel, then the
// committing action). True while open: call ImGui::EndPopup() then, as with BeginPopupModal.
inline constexpr float kModalNarrow = 440.0f, kModalWide = 560.0f;
bool BeginModal(const char* id, float widthUnits, bool* open = nullptr);
// A dialog's footer: CardDivider, then its buttons on the right in one row (each under the other, right-aligned, when they
// do not fit). Call ModalFooter with the first and the last button's widths (ButtonWidth), draw the first, then
// ModalLastAction(lastWidth, oneRow) and the last. Inside a ControlSizeScope(Compact): footers use the compact size.
// The committing action is last, a primary button with its icon.
bool ModalFooter(float firstWidth, float lastWidth);
void ModalLastAction(float lastWidth, bool oneRow);
// A feature page's header: PageTitle with the feature's own controls on the title line, right-aligned with the cards'
// switches below — [cost chip] [hold to compare] [extra toggle button] [switch]. While the switch is off the subtitle
// reads offSubtitle (when given). In the search results it is the feature's switch row (title, subtitle). True on the
// frame the switch was clicked. Every page whose feature can be turned off uses it: the switch never lives in a card.
bool PageHeader(const char* title, const char* subtitle, bool* toggle, bool toggleEnabled = true, const char* tooltip = nullptr,
                struct HeaderExtra* extra = nullptr, const char* chip = nullptr, const char* offSubtitle = "Off · your settings are kept");
// Muted wrapped text
void MutedText(const char* text);
// One line of a list: a small muted dot in the middle of the first line, the words (muted, wrapped) beside it.
// translate: look `text` up (false for text that is already the player's language or not to be translated)
void BulletLine(const char* text, bool translate = true);
// Steps in order: a numbered bubble each (joined by a thin line), the words wrapped beside it. A translated step that
// starts with its own "1. " is shown without it (the bubble says the number).
void NumberedSteps(const char* const* steps, int count);
// A small turning arc on the text line, for work that is still running; the next item goes on the same line
void BusySpinner(bool frameLine = false); // frameLine: centred on a line of buttons, else on a line of text
// Group label inside a card: regular menu font, muted, without letter spacing
void GroupLabel(const char* text);
// A quiet label that opens a group of rows inside a card ("What stays sharp"): the row divider above it when it follows
// a row, small muted text, and no divider between it and the group's first row
void CardGroup(const char* text);
// The same quiet label with a small chevron that collapses its group (open unless openByDefault is false; the state is
// kept per id while the game runs). True when open: submit the group's rows. The search results draw no label and
// return true.
bool CardGroupToggle(const char* id, const char* text, bool openByDefault = true);
// A note: a small icon and wrapped text in a subtly tinted rounded box, in the given colour (0xRRGGBB). Info notes use
// IconId::Info and the default muted colour, warnings IconId::TriangleAlert and VioletTheme::kWarning, errors
// IconId::TriangleAlert and VioletTheme::kError. The box is one item: hover it for a Tooltip.
void IconNote(IconId icon, const char* text, unsigned rgb = VioletTheme::kTextMuted);
// A note with its action inside it, on the right ("Color is off" with Turn on Color): the words wrap beside the icon,
// a compact primary button centred on the note's height. True when the button was clicked. enabled: the button's state.
bool NoteAction(IconId icon, const char* text, const char* action, IconId actionIcon, bool enabled = true,
                unsigned rgb = VioletTheme::kTextMuted);
// Short read-only facts side by side (one under the other when narrow): each a small muted label over its value, both
// translated. Not a setting row: hidden in the search results like notes
void FactRow(const char* const* labels, const char* const* values, int count);
// A small vertical gap
void Gap(float units = 4.0f);

// ---- controls ----
// Scope a whole form/action group so its inputs and buttons share one height.
// Compact is the default; Primary is for main actions, independent of button colour.
enum class ControlSize { Compact, Primary };
class ControlSizeScope {
public:
    explicit ControlSizeScope(ControlSize size);
    ~ControlSizeScope();
    ControlSizeScope(const ControlSizeScope&) = delete;
    ControlSizeScope& operator=(const ControlSizeScope&) = delete;
};

// Native-style diagnostic checkbox: a 20-unit box, independent of input frame heights.
// Labels are supplied by the caller (developer English or already translated).
bool Checkbox(const char* label, bool* value);
// Diagnostic integers retain native integer behavior inside a measured, responsive control row.
bool DiagnosticIntRow(const char* label, int* value, int min, int max, const char* description = nullptr, ImGuiSliderFlags flags = 0);

// iOS-style switch (violet when on). True on the frame it was clicked (*v is already flipped).
bool ToggleSwitch(const char* id, bool* v);
ImVec2 ToggleSwitchSize(); // Shared geometry for custom right-aligned action clusters.
// A row: label on the left with its description (a short sentence, always visible, muted and smaller) under it, the
// switch on the right. The parameter keeps its old name: the text is the description, not a hover tooltip. True when *v
// changed (also by its Reset button). def = the default (changed dot + Reset). icon is supported but the menu's rows
// have none (icons live on card headers, sidebar, notes and buttons).
bool SwitchRow(const char* label, bool* v, const char* tooltip = nullptr, BoolDefault def = {}, IconId icon = IconId::None);
// A row with custom controls on the right: label and description on the left (wrapped before the controls), then the
// cursor is placed at the right, vertically centred, for controlsWidth of controls one frame high (ImGui::SameLine
// between them; AlignTextToFramePadding before text). Returns false (and draws nothing) when the search filter hides
// it: then submit nothing and do NOT call EndControlRow(). When it returns true, always call EndControlRow(). Not
// nestable.
bool BeginControlRow(const char* label, const char* description, float controlsWidth, IconId icon = IconId::None, float controlsHeight = 0.0f);
void EndControlRow();
// The next BeginControlRow has a default: the changed dot while `changed`, and on hover the row's Reset button.
// ControlRowReset(): that Reset was clicked (read between BeginControlRow and EndControlRow; the caller puts the value
// back and reports it). The caller gives the row a unique ID scope (PushID).
void SetNextRowDefault(bool changed);
bool ControlRowReset();
// A colour row (a control row): a swatch with the colour's 0..255 values beside it; clicking the swatch opens a colour
// picker (with an alpha bar when comps is 4). rgba holds comps floats in 0..1; def (optional) = the default (changed
// dot + Reset). True when the colour changed.
bool ColorRow(const char* label, const char* description, float* rgba, int comps, const float* def = nullptr);
// A long list draws only the rows it shows: a row outside the view advances the layout exactly as BeginControlRow +
// EndControlRow did with the same label and description, by its height measured then (ControlRowHeight, read before
// EndControlRow), without drawing it.
void SkipControlRow(const char* label, const char* description, float height);
float ControlRowHeight();
// The last control row: its top (screen y) and whether its controls went under the text (narrow)
void LastControlRow(float* topScreenY, bool* stacked);
// A small muted chip ("~0.4 ms"); submitted as an item (hover it for a tooltip)
ImVec2 ChipSize(const char* text);
void Chip(const char* text, unsigned rgb = 0x8B8C96);
// Figures side by side in equal columns: a small muted label (translated) over its value (shown as it is), each value in
// its colour (nullptr = the text colour). For the Overview tiles.
void StatColumns(const char* const* labels, const char* const* values, const unsigned* colours, int count);
// The Overview's status halves (Frame rate, Game memory), drawn with the same metrics so their rows line up side by side:
//   StatusHeader  small muted label on the left; optional small text on the right, in its colour, after a dot
//   StatusValue   the value large and bold, its unit small and muted on the same baseline; returns the line's top-left
//                 (screen) and its height, for a drawing beside it
//   StatusBand    the 4-unit band under the value (Game memory draws its bar there; Frame rate leaves it empty)
//   StatusFooter  one small muted line, cut with an ellipsis when narrow
void StatusHeader(const char* label, const char* right = nullptr, unsigned rightRgb = 0x8B8C96, bool dot = false);
float StatusValue(const char* value, const char* unit, ImVec2* lineMin = nullptr);
ImVec2 StatusBand(); // returns the band's top-left (screen); its height is StatusBandHeight()
float StatusBandHeight();
void StatusFooter(const std::string& text);
// A rounded pill with an optional icon; highlighted = violet. Submitted as an item (hover it for a tooltip).
ImVec2 PillSize(const char* text, bool withIcon);
void Pill(const char* text, bool highlighted, IconId icon = IconId::None);
// Borderless square button with an icon (header close, before/after); true when clicked. `active` keeps it violet.
bool IconButton(const char* id, IconId icon, const char* tooltip = nullptr, bool active = false, float sizeUnits = 24.0f);
// Buttons use the active compact/primary group frame height. Secondary = neutral fill with a border that turns violet on hover (Reset, Save, Change);
// Primary = violet fill with white text, for the key action of a card (Turn on ..., Download).
// Danger = red fill with white text, only for the confirming button of a removal.
enum class ButtonKind { Secondary, Primary, Danger };
// A button with a 20-unit icon before its label; true when clicked. "Label##id" hides the ##id part.
bool IconTextButton(const char* label, IconId icon, const char* tooltip = nullptr, ButtonKind kind = ButtonKind::Secondary);
// The same without an icon; minWidth (pixels, already x Unit()) for equal-width buttons like - / +.
bool TextButton(const char* label, const char* tooltip = nullptr, ButtonKind kind = ButtonKind::Secondary, float minWidth = 0.0f);
// Width of those buttons (for BeginControlRow)
float ButtonWidth(const char* label, bool withIcon, float minWidth = 0.0f);
// A custom control's closed text cut with "…" to maxW like the select rows (true when cut), and the full-text tooltip
bool FitClosedText(std::string_view text, float maxW, std::string& out);
void ShowFullText(std::string_view text);
// Sidebar entry: icon + label, filled background and a violet bar when selected. True when clicked. collapsed = the
// icon-only rail (the label becomes the hover tooltip).
// badge: a small chip at the item's right ("Beta"); with the sidebar collapsed it joins the tooltip
bool SidebarItem(IconId icon, const char* label, bool selected, bool collapsed = false, const char* badge = nullptr);
// The width a sidebar item needs to show its whole label (and badge) in the menu's language, at today's size
float SidebarItemWidth(const char* label, const char* badge = nullptr);
// Sidebar group label ("WORLD"): the GroupLabel look, aligned with the items' icons, with kSpace3 above it; collapsed =
// a short hairline instead of the text
void SidebarGroup(const char* text, bool collapsed = false);
// Underline tab bar at the top of a page: text with an optional small icon; the selected tab is white with a 2 px violet
// underline, the others muted (kText on hover); a hairline under the whole bar. Wraps onto another line when the tabs do
// not fit. kSpace3 of space follows it. True when the user picked another tab (*current is already set).
bool TabBar(const char* id, int* current, const char* const* labels, int count, const IconId* icons = nullptr);
// Segmented control: one row of choices (a vertical list when they do not fit the width). *current = index of the
// selected one (-1 = none of them, e.g. a custom value). True when the user picked another one. `compact` = the compact
// tab look used for sub-pages (Picture > Advanced). icons (optional, IconId::None for none) go before the labels.
// tooltips show on hover of each choice.
bool Segmented(const char* id, int* current, const char* const* labels, int count, const char* const* tooltips = nullptr, bool compact = false,
               const IconId* icons = nullptr);
// A row: label, description under it, then the segmented control on the full width. True when the choice changed (also
// by its Reset button). defaultIndex = the default choice (changed dot + Reset), kNoDefaultIndex = none.
bool SegmentedRow(const char* label, const char* description, const char* id, int* current, const char* const* labels, int count,
                  const char* const* tooltips = nullptr, const IconId* icons = nullptr, int defaultIndex = kNoDefaultIndex);
// The next SegmentedRow gets a small icon button right of its choices, one frame high (Capture's "Open the folder");
// consumed by that row, drawn or not. RowActionClicked: it was clicked in the last SegmentedRow.
void SetNextRowAction(IconId icon, const char* tooltip);
bool RowActionClicked();
// Standard single-choice row: label and description on the left, a dropdown on the right, with the same changed
// marker, hover Reset and search behavior as other setting rows. defaultIndex = the default choice.
// stacked puts the same selector below its label at full width for file and preset dialogs.
bool SelectRow(const char* label, const char* description, const char* id, int* current, const char* const* labels, int count,
               float controlWidth = 220.0f, int defaultIndex = kNoDefaultIndex, bool stacked = false);
// Whether the last SelectRow's dropdown list was open this frame (the language row merges every script's font then)
bool SelectRowOpen();
// The menu's dropdown on its own (SelectRow's closed control, for a list built by hand): `preview` (already the player's
// language) cut with "…" before the chevron and shown whole on hover; width in pixels, -1 = the rest of the line. While it
// returns true, submit ImGui::Selectable items, then ImGui::EndCombo().
bool BeginDropdown(const char* id, const char* preview, float width);

// "Advanced" section: a hairline, then a violet chevron row, collapsed by default (the state is kept per id while the
// game runs). The contents are not indented and use the same rows. When it returns true, submit the contents and call
// EndAdvanced().
// summary (optional, already translated): what is chosen inside, muted after the label ("Advanced  ·  PNG, 100%")
bool BeginAdvanced(const char* id, const char* label = "Advanced", const char* summary = nullptr);
void EndAdvanced();
// The old tree-node look (still used by the Frame Profiler); same contract as ImGui::TreeNode (TreePop when true).
bool AdvancedNode(const char* label);

// Styled slider row: label on the left and the value (muted) on the right on one line, the description under them, then a
// thin violet track with a white grab. The stored value is *v in [min, max]; what is shown is valueText, else format
// applied to *v x displayScale + displayOffset. leftLabel / rightLabel are muted end labels under the track ("Pink" ...
// "Warm white"); swatch (0 = none) draws a small colour square before the value; icon goes before the label (unused by
// the menu). `tooltip` is the description (always visible). defaultValue = the default (changed dot + Reset; kNoDefault =
// none). The track is violet up to the grab, or a colour gradient (trackFrom -> trackTo) or the hue circle (hueTrack,
// the range is 0..360 degrees) over its whole length. Same return value as ImGui::SliderFloat, plus true when its Reset
// button was clicked; SliderCommitted() is IsItemDeactivatedAfterEdit of the last slider (or its Reset).
struct SliderOptions {
    const char* format = "%.2f";
    float displayScale = 1.0f;
    float displayOffset = 0.0f;
    const char* tooltip = nullptr;
    const char* leftLabel = nullptr;
    const char* rightLabel = nullptr;
    const char* valueText = nullptr;
    ImU32 swatch = 0;
    IconId icon = IconId::None;
    float defaultValue = kNoDefault;
    ImU32 trackFrom = 0, trackTo = 0; // both set = gradient track
    bool hueTrack = false;
    bool markDefault = false; // a small amber tick on the track at defaultValue (Looks: the .ini's value)
    // > 0: the track takes the column of a SelectRow with this controlWidth (aligned with the select above or below
    // it) and the value moves to the track's left
    float alignWidth = 0.0f;
};
bool Slider(const char* label, float* v, float min, float max, const SliderOptions& options = {});
bool SliderCommitted();
// Shorthand: percentage display (*v x 100, "%.0f%%"); tooltip = the description; def = the default
bool SliderPercent(const char* label, float* v, float min, float max, const char* tooltip = nullptr, float def = kNoDefault, IconId icon = IconId::None);
// Numbers in the menu language's form: a decimal point between digits becomes a comma where the language writes one
// ("1.66 GB" reads "1,66 GB" in Portuguese, German, French...). Edits s in place.
void LocaleDecimal(char* s);
// The shared control column's width for a row as wide as the space left here (pixels; 0 when too narrow): where
// sliders, dropdowns and segmented choices start, for custom controls that should line up with them
float ControlColumnWidth();
// A search field across the space left (the menu header's look in a card): the search icon inside its left end, the hint
// (translated), and a clear button inside its right end while it has text. True when the text changed.
bool SearchField(const char* id, const char* hint, char* buf, size_t size);
// The previous signature (label, value, range, format)
bool SliderFloat(const char* label, float* v, float min, float max, const char* format = "%.2f");

// ---- cards ----
// A feature card: rounded panel, card colour, 1 px border, padding kSpace4 x kSpace3, auto height, kSpace3 below it.
// ALWAYS call EndCard(), whatever BeginCard returned; submit the contents only when it returned true (false = scrolled
// out of view).
bool BeginCard(const char* id, bool warning = false);
void EndCard();
// Between a card's header and its body: a hairline with kSpace3 above and below. Call it only when a body follows.
void CardDivider();
// An optional small toggle button left of the header's switch (Picture's before / after), and optionally a "hold"
// icon button left of it (Picture's hold to compare: *held is true while the mouse button is held on it).
struct HeaderExtra {
    IconId iconOff = IconId::None;
    IconId iconOn = IconId::None;
    bool* value = nullptr;
    bool enabled = true;
    const char* tooltip = nullptr;
    bool clicked = false; // set by CardHeader
    IconId holdIcon = IconId::None; // None = no hold button
    const char* holdTooltip = nullptr;
    bool held = false; // set by CardHeader
    const char* badge = nullptr; // optional warning-colour chip immediately left of the header switch
    const char* badgeTooltip = nullptr;
    const char* chipTooltip = nullptr; // the cost chip's tooltip (null: kCostChipTooltip)
};
// Tooltip of the performance chips
inline constexpr const char* kCostChipTooltip = "Measured cost on your GPU per frame";
// The card's header row: violet icon, bold title, muted subtitle, the tooltip on hover (the feature description) and,
// when toggle is not null, the switch on the right (greyed out and inert when toggleEnabled is false; the title stays
// readable). chip (optional) = a small muted chip left of the switch (the GPU cost, "~0.4 ms"). HeaderExtra::badge
// (optional) draws a warning-colour label immediately left of the switch. True on the frame the switch was clicked.
bool CardHeader(IconId icon, const char* title, const char* subtitle, const char* tooltip, bool* toggle, bool toggleEnabled = true,
                HeaderExtra* extra = nullptr, const char* chip = nullptr);
// A card header with a live status and one action (Image > Capture): violet icon, bold title (translated), the status
// line under it as given (already translated) in statusRgb, with a small dot of that colour before it when dot, an
// optional text link after it (translated; on its own line when it does not fit), and a compact button on the right
// (under the text when the card is narrow; greyed out when !actionEnabled). Draws nothing in the search results.
// Returns 1 when the button was clicked, 2 when the link was, else 0.
int CardStatusHeader(IconId icon, const char* title, const char* status, unsigned statusRgb, bool dot, const char* link, const char* action,
                     IconId actionIcon, ButtonKind kind, bool actionEnabled);
// An overview row (Overview page, a list inside one card): icon, name (clickable: *nameClicked), muted phrase under it,
// and on the right the switch (when on is not null) or a pill with rightText; chip (optional) left of them. Consecutive
// rows get dividers. True on the frame the switch was clicked (it reports "<name> turned on/off").
bool OverviewRow(const char* id, IconId icon, const char* name, const char* phrase, const char* tooltip, bool* on, bool enabled, const char* rightText,
                 bool* nameClicked, const char* chip = nullptr, const char* summary = nullptr, bool attention = false);
// Text of a GPU cost chip ("~0.4 ms") into buf; false (buf empty) when not measured (ms < 0)
bool CostChipText(float ms, char* buf, int size);

} // namespace ApexUi

namespace ApexUi {
bool ProfileChoiceRow(const char* id, IconId icon, const char* name, const char* description, bool selected);
// Identified action popup. Title is already translated (or a player-entered name).
bool BeginActionMenu(const char* id, const char* title, float minimumWidth = 210.0f);
bool ActionMenuItem(const char* label, IconId icon, bool enabled = true);
void EndActionMenu();
}
