# EPub-InkPlate Event Map

This document maps every interactive surface and the events it handles. Each viewer and controller processes input events, dispatching them based on the EventKind enum.

## Event Kinds

### Touch Screen Devices (INKPLATE_6PLUS, INKPLATE_6PLUS_V2, INKPLATE_6FLICK, INKPLATE_5V2, TOUCH_TRIAL)
- **TAP**: Single finger tap/click at position (x, y)
- **HOLD**: Finger held down for 500ms at position (x, y)
- **RELEASE**: Finger released after a hold
- **SWIPE_LEFT**: Finger swiped left (distance > 30 pixels)
- **SWIPE_RIGHT**: Finger swiped right (distance > 30 pixels)
- **PINCH_ENLARGE**: Two fingers pinched apart (zoom in), distance tracked
- **PINCH_REDUCE**: Two fingers pinched together (zoom out), distance tracked
- **WAKEUP_BUTTON**: Wake button pressed (global event)

### Keypad Devices (INKPLATE_5 and others)
- **NEXT**: Next button pressed (or DBL_NEXT depending on EXTENDED_CASE)
- **PREV**: Previous button pressed (or DBL_PREV depending on EXTENDED_CASE)
- **DBL_NEXT**: Double/long press of next button
- **DBL_PREV**: Double/long press of previous button
- **SELECT**: Select button pressed
- **DBL_SELECT**: Double/long press of select button
- **NONE**: No event

---

## AppController

**Purpose**: Top-level event dispatcher that routes input to the active controller (DIR, BOOK, PARAM, OPTION, TOC)

**Reached from**: Application startup; controllers transition back via `set_controller()`

**Events handled**:
- **PINCH_ENLARGE** (touch devices only): Adjusts backlight brightness up by `event.dist` via `back_lit.adjust()`
- **PINCH_REDUCE** (touch devices only): Adjusts backlight brightness down by `event.dist` via `back_lit.adjust()`
- **WAKEUP_BUTTON** (all devices): Powers off the device via `CommonActions::power_it_off()`

**Notes**: 
- AppController intercepts global backlight and power events before delegating to the active controller
- Touch device specific: requires INKPLATE_6PLUS or INKPLATE_6PLUS_V2 or INKPLATE_6FLICK
- Different build defines affect which events are available

---

## BookController

**Purpose**: Manages book reading interaction; handles page navigation

**Reached from**: Books directory controller when a book is selected (via `app_controller.set_controller(AppController::Ctrl::BOOK)`)

**Events handled** (touch devices):
- **SWIPE_RIGHT**: 
  - If tap in upper area (y < height - 40): Loads previous page via `page_locs.get_prev_page_id()`
  - If tap in bottom area (y >= height - 40): Loads 10 pages back via `page_locs.get_prev_page_id(..., 10)`
  - Updates `current_page_id` and calls `book_viewer.show_page()`
- **SWIPE_LEFT**:
  - If tap in upper area (y < height - 40): Loads next page via `page_locs.get_next_page_id()`
  - If tap in bottom area (y >= height - 40): Loads 10 pages forward via `page_locs.get_next_page_id(..., 10)`
  - Updates `current_page_id` and calls `book_viewer.show_page()`
- **TAP**:
  - Left third of screen (x < width/3): Loads previous page
  - Right two-thirds of screen (x > 2*width/3): Loads next page
  - Middle third or bottom area (y >= height - 40): Transitions to PARAM controller (book settings)

**Events handled** (keypad devices):
- **PREV** (or DBL_NEXT if EXTENDED_CASE): Loads next page
- **DBL_PREV** (or PREV if EXTENDED_CASE): Loads 10 pages forward
- **NEXT** (or DBL_NEXT if EXTENDED_CASE): Loads next page
- **DBL_NEXT** (or NEXT if EXTENDED_CASE): Loads 10 pages forward
- **SELECT** or **DBL_SELECT**: Transitions to PARAM controller (book settings)

**Notes**: 
- Page navigation respects the bottom status bar area (40 pixels high) for 10-page jumps
- Behavior differs based on EXTENDED_CASE compile flag for keypad devices

---

## BooksDirController

**Purpose**: Manages book directory browsing; lists available eBooks

**Reached from**: Application startup (default) or from PARAM/OPTION menus

**Events handled** (touch devices):
- **SWIPE_RIGHT**: Goes to previous page via `books_dir_viewer->prev_page()`
- **SWIPE_LEFT**: Goes to next page via `books_dir_viewer->next_page()`
- **TAP**:
  - If matrix view OR in left third of screen (x < width/3): Tap on book or menu area
    - On valid book: Opens book via `book_controller.open_book_file()`, transitions to BOOK controller
    - On menu area (invalid): Transitions to OPTION controller
  - If in right two-thirds (linear view): Transitions to OPTION controller
- **HOLD**: Highlights book at (x, y) via `books_dir_viewer->highlight_book()`
- **RELEASE**: Clears highlight via `books_dir_viewer->clear_highlight()`, 1 second delay

**Events handled** (keypad devices):
- **PREV** (or DBL_PREV if EXTENDED_CASE): Moves to previous column via `books_dir_viewer->prev_column()`
- **NEXT** (or DBL_NEXT if EXTENDED_CASE): Moves to next column via `books_dir_viewer->next_column()`
- **DBL_PREV** (or PREV if EXTENDED_CASE): Moves to previous item via `books_dir_viewer->prev_item()`
- **DBL_NEXT** (or NEXT if EXTENDED_CASE): Moves to next item via `books_dir_viewer->next_item()`
- **SELECT**: Opens currently selected book, transitions to BOOK controller
- **DBL_SELECT**: Transitions to OPTION controller

**Notes**: 
- Stores last read book index and position
- On enter, can auto-show the last read book if configured
- Uses either LinearBooksDirViewer or MatrixBooksDirViewer based on DIR_VIEW setting
- Touch devices distinguish between book area and menu area via coordinates

---

## BookParamController

**Purpose**: Manages book-specific settings and options menu

**Reached from**: BOOK controller (center/bottom tap), OPTION/PARAM menus

**Events handled** (in priority order):
- **Form shown** (book_params_form_is_shown = true):
  - Delegates to `form_viewer.event(event)`
  - On completion: Applies form changes to book parameters, saves to epub, refreshes fonts if needed
- **Delete confirmation** (delete_current_book = true):
  - Delegates to `msg_viewer.confirm(event, ok)`
  - If OK: Deletes book file, companion .pars/.locs/.toc files, refreshes directory, returns to DIR
  - If CANCEL: Cancels deletion
- **Post-WiFi restart** (wait_for_key_after_wifi = true):
  - Any key: Shows restart message, restarts device
- **Menu shown** (default state):
  - Delegates to `menu_viewer.event(event)`
  - Menu actions:
    - Return: Goes back to BOOK controller via `app_controller.set_controller(Ctrl::LAST)`
    - Table of Content: Goes to TOC controller
    - E-Books list: Goes to DIR controller
    - Current e-book parameters: Shows form_viewer with book parameters
    - Revert parameters: Reverts to defaults, reloads fonts if needed
    - Delete the current e-book: Shows confirmation dialog
    - WiFi Access: Starts web server (device won't sleep)
    - About: Shows info dialog
    - Power OFF: Saves state and deep sleeps

**Notes**: 
- Form completion applies all changes immediately
- Font changes trigger immediate reload or clear
- WiFi mode keeps device awake until restart
- Form (DONE button) returns with `app_controller.set_controller(Ctrl::LAST)`

---

## OptionController

**Purpose**: Manages global application settings

**Reached from**: Books directory via menu or confirmation dialogs

**Events handled** (in priority order):
- **Main form shown** (main_form_is_shown = true):
  - Delegates to `form_viewer.event(event)`
  - On completion: Saves orientation, view mode, resolution, battery display, title, timeout settings
  - Side effects: May recalculate page locations, refresh fonts, update screen orientation
- **Font form shown** (font_form_is_shown = true):
  - Delegates to `form_viewer.event(event)`
  - On completion: Saves default font size, font, images flag, font usage setting
- **Date/Time form shown** (date_time_form_is_shown = true, only if DATE_TIME_RTC):
  - Delegates to `form_viewer.event(event)`
  - On completion: Calls `set_clock()` to update system time
- **Post-WiFi restart** (wait_for_key_after_wifi = true):
  - Any key: Shows restart message, optionally refreshes books directory if needed, restarts
- **Calibration shown** (calibration_is_shown = true, touch devices only):
  - Delegates to `event_mgr.calibration_event(event)`
  - On completion: Saves calibration, returns to menu
- **Menu shown** (default state):
  - Delegates to `menu_viewer.event(event)`
  - Menu actions:
    - Return to books list: Goes to DIR controller via `Ctrl::LAST`
    - Return to last e-book: Goes to DIR controller, auto-shows last book
    - Main parameters: Shows main_params_form
    - Default e-books parameters: Shows font_params_form
    - WiFi Access: Starts web server in STA mode
    - Refresh e-books list: Rescans books directory
    - Clear e-books history (keypad devices only): Initializes NVS
    - Set Date/Time (RTC devices): Shows date_time_form
    - Retrieve Date/Time from Time Server: Connects to NTP, updates clock
    - About: Shows info dialog
    - Power OFF: Powers down device
    - Other options (touch devices): Shows sub_menu

**Notes**: 
- Has two menus: main menu and optional sub_menu for touch devices
- Forms include DONE button for touch completion
- Orientation/resolution changes trigger page location recalculation
- WiFi mode keeps device awake with event_mgr.set_stay_on(true)

---

## TocController

**Purpose**: Manages Table of Contents navigation

**Reached from**: PARAM controller (book menu option)

**Events handled** (touch devices):
- **SWIPE_RIGHT**: Goes to previous page via `toc_viewer.prev_page()`
- **SWIPE_LEFT**: Goes to next page via `toc_viewer.next_page()`
- **TAP**:
  - On valid TOC entry with valid page offset: Sets book page via `book_controller.set_current_page_id()`, transitions to BOOK
  - On invalid entry: Transitions back to BOOK controller
- **HOLD**: No action (empty handler)
- **RELEASE**: Clears highlight via `toc_viewer.clear_highlight()`

**Events handled** (keypad devices):
- **PREV** (or DBL_PREV if EXTENDED_CASE): Goes to previous column
- **NEXT** (or DBL_NEXT if EXTENDED_CASE): Goes to next column
- **DBL_PREV** (or PREV if EXTENDED_CASE): Goes to previous item
- **DBL_NEXT** (or NEXT if EXTENDED_CASE): Goes to next item
- **SELECT**: Opens selected TOC entry and transitions to BOOK controller
- **DBL_SELECT**: Transitions back to BOOK controller

**Notes**: 
- Only shown if TOC is ready and not empty (checked at menu entry level)
- Uses page-based and item-based navigation on keypad devices
- TAP on invalid entries safely returns to book view

---

## MenuViewer

**Purpose**: Renders and handles menu navigation (icons with touch and hold)

**Reached from**: Embedded in controllers that have menu options (PARAM, OPTION, etc.)

**Events handled** (touch devices):
- **HOLD**: 
  - Finds menu entry at (x, y) via `find_index()`
  - If valid: Shows caption text for the entry
  - Highlights nothing (caption display only)
- **RELEASE**: 
  - Clears any displayed caption
  - 1 second delay before clearing
- **TAP**:
  - Finds menu entry at (x, y) via `find_index()`
  - If valid and has function: 
    - If highlighted flag set: Shows caption and highlight then executes function
    - If not highlighted: Just executes function
  - Returns false (does not exit menu)

**Events handled** (keypad devices):
- **PREV**: Moves to previous visible menu entry (wraps to end)
- **NEXT**: Moves to next visible menu entry (wraps to start)
- **DBL_PREV**: No action (returns false)
- **DBL_NEXT**: No action (returns false)
- **SELECT**: Executes function of current entry, returns false to stay in menu
- **DBL_SELECT**: Exits menu by returning true

**Notes**: 
- Entries have visibility flags; navigation skips invisible entries
- First and last entries must always be visible (architectural requirement)
- Touch devices use icon tapping with hold-to-show-caption
- Keypad devices cycle through visible entries

---

## FormViewer

**Purpose**: Renders and handles form field input (choices, numeric values, done buttons)

**Reached from**: Embedded in controllers showing parameter forms

**Events handled** (through FormField::event()):

**FormChoiceField** (touch devices):
- **TAP**: 
  - Checks all choice items at (x, y)
  - Selects tapped item, updates highlight

**FormChoiceField** (keypad devices):
- **PREV** or **DBL_PREV**: Moves to previous choice item (wraps)
- **NEXT** or **DBL_NEXT**: Moves to next choice item (wraps)

**FormDone** (touch devices only):
- Any event: Sets `form_viewer.completed = true`

**Events handled** at FormViewer level:
- Delegates to active field's `event()` method
- On form completion: Returns true to notify caller
- Caller saves form values and applies settings

**Notes**: 
- Forms include choice fields (horizontal or vertical layout)
- Numeric fields for touch devices (UINT16 type)
- DONE button appears only on touch devices
- Keypad devices exit forms via field navigation

---

## MsgViewer

**Purpose**: Renders and handles modal message dialogs and confirmations

**Reached from**: Embedded in controllers showing alerts, confirmations, or progress

**Events handled**:

**Confirmation dialogs** (when confirm() called):
- **TAP** (touch devices):
  - On OK button bounds: Sets ok=true, returns true
  - On CANCEL button bounds: Sets ok=false, returns true
- **SELECT** (keypad devices): Sets ok=true, returns true
- Any other event: Returns false (dialog stays open)

**Info/Alert dialogs** (when show() called with press_a_key=true):
- **TAP** (touch devices): Any tap dismisses (info messages only)
- Any keypress (keypad devices): Dismisses dialog

**Notes**: 
- Confirmation dialogs show OK and CANCEL buttons side by side
- Info messages show "[Please TAP the screen]" on touch devices
- Center-aligned, bordered dialog box
- Cannot be used during location computation mode

---

## KeypadViewer (Numeric Input)

**Purpose**: Displays on-screen numeric keypad for input form fields

**Reached from**: Embedded in FormViewer for numeric field input

**Events handled** (keypad devices via navigation):
- **LEFT/UP/DOWN/RIGHT**: Navigates between keypad buttons (via parent form)
- **SELECT**: Presses the highlighted button

**Button functions**:
- Digit buttons (0-9): Appends digit to input
- BackSpace button: Removes last digit
- Clear button: Clears all input
- OK button: Confirms and returns value
- CANCEL button: Cancels input

**Notes**: 
- Touch devices tap directly on keys
- Keypad displays 14 keys total (0-9, BackSpace, Clear, OK, CANCEL)
- Maximum 4 digit input
- Cancel button is wider (double width)

---

## LinearBooksDirViewer & MatrixBooksDirViewer

**Purpose**: Display books directory in list or grid format

**Reached from**: Embedded in BooksDirController

**Events handled** (none directly; controller calls viewer methods):
- Navigation methods called by BooksDirController:
  - `next_page()`: Shows next page of books
  - `prev_page()`: Shows previous page of books
  - `next_item()`: Highlights next item (keypad mode)
  - `prev_item()`: Highlights previous item (keypad mode)
  - `next_column()`: Moves to next column (keypad mode, matrix view)
  - `prev_column()`: Moves to previous column (keypad mode, matrix view)
  - `get_index_at(x, y)`: Returns book index at touch coordinates
  - `highlight_book(idx)`: Updates highlight for given index
  - `clear_highlight()`: Removes highlight

**Notes**: 
- LinearBooksDirViewer shows books as vertical list with cover + title/author
- MatrixBooksDirViewer shows books as grid (columns × rows)
- Both track current page and highlighted item
- Used by BooksDirController to handle navigation

---

## TocViewer

**Purpose**: Displays Table of Contents entries

**Reached from**: Embedded in TocController

**Events handled** (none directly; controller calls viewer methods):
- Navigation methods called by TocController:
  - `next_page()`: Shows next page of TOC entries
  - `prev_page()`: Shows previous page of TOC entries
  - `next_item()`: Highlights next entry
  - `prev_item()`: Highlights previous entry
  - `next_column()`: Moves to next column (if multi-column layout)
  - `prev_column()`: Moves to previous column
  - `get_index_at(x, y)`: Returns entry index at touch coordinates
  - `show_page_and_highlight(idx)`: Shows page containing entry and highlights it
  - `clear_highlight()`: Removes highlight

**Notes**: 
- Displays hierarchical chapter/section structure
- Each entry has a page_id (itemref_index and offset)
- Invalid entries (offset < 0) cannot be opened

---

## BookViewer

**Purpose**: Renders e-book page content

**Reached from**: Embedded in BookController

**Events handled**: None directly (does not handle events)

**Methods called by BookController**:
- `show_page(page_id)`: Renders and displays page at given location
- `init()`: Initializes viewer for current book

**Notes**: 
- Pure display component; all navigation handled by BookController
- Uses Page class for rendering formatted text/images
- Updates are triggered by BookController event handlers

---

## Touch Event Generation (EventMgr)

**State machine** for touch event generation:

**States**:
- **NONE**: Waiting for any touch
  - Single touch → go to WAIT_NEXT, record start position
  - Two finger touch → go to PINCHING
- **WAIT_NEXT**: Single touch detected, waiting for movement or release
  - Release after 500ms timeout → TAP event
  - Movement > 30px distance → go to SWIPING
  - Second finger added → go to PINCHING
- **HOLDING**: Single touch held down (no movement)
  - Release or timeout → RELEASE event
- **SWIPING**: Finger moving (distance > 30px)
  - Release → SWIPE_LEFT or SWIPE_RIGHT event (based on x direction)
  - Second finger added → go to PINCHING
  - Timeout → SWIPE event
- **PINCHING**: Two finger touch
  - Release → RELEASE event
  - Distance change → PINCH_ENLARGE or PINCH_REDUCE event with distance delta
  - Timeout → RELEASE event

**Global events**:
- **WAKEUP_BUTTON**: Power button press (interrupts any ongoing gesture)

**Notes**: 
- Touch coordinates converted from hardware to user coordinates
- 500ms timeout triggers transition from WAIT_NEXT to HOLDING
- 4 second timeout in HOLDING/SWIPING/PINCHING triggers release
- Distance threshold: 30 pixels for swipe detection
- Swipe direction determined by x coordinate comparison (x_end > x_start = SWIPE_RIGHT)

---

## Event Flow Summary

1. **EventMgr** captures raw touch/button input
2. **EventMgr::get_event()** returns current event to the main loop
3. **AppController::input_event()** routes to active controller
4. **Active Controller** (BOOK, DIR, PARAM, OPTION, TOC):
   - May handle event directly
   - May delegate to embedded viewer (MenuViewer, FormViewer, MsgViewer)
   - May transition to different controller via `app_controller.set_controller()`
5. **Viewers** (MenuViewer, FormViewer, MsgViewer, etc.):
   - Handle specific interactive elements
   - Return true/false to indicate completion or request

**Controller hierarchy**:
```
AppController (top level)
├── BooksDirController (browse books)
│   └── LinearBooksDirViewer or MatrixBooksDirViewer
├── BookController (read book)
│   └── BookViewer (pure display)
├── BookParamController (book settings)
│   ├── MenuViewer (main menu)
│   ├── FormViewer (parameters form)
│   ├── MsgViewer (dialogs)
│   └── KeypadViewer (numeric input)
├── OptionController (global settings)
│   ├── MenuViewer (options menu + submenu)
│   ├── FormViewer (various forms)
│   ├── MsgViewer (dialogs)
│   └── KeypadViewer (numeric input)
└── TocController (table of contents)
    └── TocViewer

```

