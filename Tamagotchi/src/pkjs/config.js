module.exports = [
  { 
    "type": "heading", 
    "defaultValue": "Tamagotchi Emu 4 Pebble" 
  }, 
  { 
    "type": "text", 
    "defaultValue": "Created by Stefan Bauwens for the Spring 2026 Pebble App Contest." 
  },
  { 
    "type": "text", 
    "defaultValue": "For more info check the readme at <a href='https://github.com/StefanBauwens/Tamagotchi-Emulator-Pebble'>https://github.com/StefanBauwens/Tamagotchi-Emulator-Pebble</a>" 
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Settings"
      },
      {
        "type": "text",
        "defaultValue": "Link to correctly formatted raw P1 ROM file"
      },
      {
        "type": "input",
        "messageKey": "ROMUrl",
        "label": "ROM URL",
        "defaultValue": "",
        "attributes": {
          "placeholder": "See github readme!"
        }
      },  
      {
        "type": "text",
        "defaultValue": "(Optional) Tamagotchi Server for running in background. Leave blank if not used."
      },
      {
        "type": "input",
        "messageKey": "APIServerUrl",
        "label": "Server",
        "defaultValue": "",
        "attributes": {
          "placeholder": "e.g. http://192.168.0.100:5000"
        }
      },
      {
        "type": "toggle",
        "messageKey": "UseEmbeddedRom",
        "label": "Use embedded ROM",
        "description": "Boot from the ROM bundled in the app (faster, works without phone). Disable to use the ROM URL from the phone instead.",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "VibrationEnabled",
        "label": "Vibrate on attention",
        "description": "Watch vibrates when the Tama needs attention (hungry, sick, ...).",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "SoundEnabled",
        "label": "Sound (experimental, Pebble Time 2/Flint only)",
        "description": "Play short beeps through the watch speaker when the Tama buzzes. Off by default — may be unstable on some firmware versions.",
        "defaultValue": false
      },
      {
        "type": "slider",
        "messageKey": "SoundVolume",
        "label": "Sound volume",
        "defaultValue": 60,
        "min": 0,
        "max": 100,
        "step": 10
      },
      {
        "type": "toggle",
        "messageKey": "reset_tamagotchi",
        "label": "Reset Tamagotchi on Save",
        "defaultValue": false
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Watchface customization (Pebble Time 2)"
      },
      {
        "type": "text",
        "defaultValue": "Change the colors of the clock face. Defaults: white text and hands with black outline."
      },
      {
        "type": "color",
        "messageKey": "TextColor",
        "label": "Text color (time, date, battery)",
        "defaultValue": "0xFFFFFF",
        "sunlight": false
      },
      {
        "type": "toggle",
        "messageKey": "TextOutline",
        "label": "Text outline",
        "description": "Adds a contrasting outline around the text — helps readability over varied backgrounds.",
        "defaultValue": true
      },
      {
        "type": "color",
        "messageKey": "TextOutlineColor",
        "label": "Text outline color",
        "defaultValue": "0x000000",
        "sunlight": false
      },
      {
        "type": "color",
        "messageKey": "HandsColor",
        "label": "Clock hands color",
        "defaultValue": "0xFFFFFF",
        "sunlight": false
      },
      {
        "type": "color",
        "messageKey": "HandsOutlineColor",
        "label": "Clock hands outline color",
        "defaultValue": "0x000000",
        "sunlight": false
      },
      {
        "type": "select",
        "messageKey": "HandsThickness",
        "label": "Clock hands thickness",
        "defaultValue": "1",
        "options": [
          { "label": "Thin", "value": "0" },
          { "label": "Normal", "value": "1" },
          { "label": "Thick", "value": "2" }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "BgUseCustom",
        "label": "Use custom watchface background",
        "description": "When OFF, use the built-in bgEmery.png. When ON, draw the background and hour markers in code so you can pick colors and style below. Takes effect after restarting the app.",
        "defaultValue": false
      },
      {
        "type": "color",
        "messageKey": "BgFillColor",
        "label": "Watchface background color",
        "description": "Only used when 'Use custom watchface background' is ON.",
        "defaultValue": "0x000000",
        "sunlight": false
      },
      {
        "type": "color",
        "messageKey": "BgMarkersColor",
        "label": "Hour markers color",
        "description": "Only used when 'Use custom watchface background' is ON.",
        "defaultValue": "0xFFFFFF",
        "sunlight": false
      },
      {
        "type": "select",
        "messageKey": "BgMarkersStyle",
        "label": "Hour markers style",
        "description": "Only used when 'Use custom watchface background' is ON.",
        "defaultValue": "0",
        "options": [
          { "label": "Arabic numerals (1, 2, 3...)", "value": "0" },
          { "label": "Roman numerals (I, II, III...)", "value": "1" },
          { "label": "Tick marks", "value": "2" }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "IconsSmall",
        "label": "Compact menu icons",
        "description": "When ON, use smaller (18x15) menu icons for a tighter look. Takes effect after restarting the app.",
        "defaultValue": false
      },
      {
        "type": "toggle",
        "messageKey": "TamaBgEnabled",
        "label": "Show Tama background",
        "description": "When OFF, the white/colored rounded background behind the Tama and menu icons disappears, showing the watchface background directly.",
        "defaultValue": true
      },
      {
        "type": "color",
        "messageKey": "TamaBgColor",
        "label": "Tama background color",
        "description": "Color of the rounded area behind the Tama LCD and menu icons. Only used when 'Show Tama background' is ON.",
        "defaultValue": "0xFFFFFF",
        "sunlight": false
      },
      {
        "type": "color",
        "messageKey": "TamaPixelColor",
        "label": "Tama pixel & icons color",
        "description": "Color for the Tama LCD pixels and the menu icons. Default black. Pick white for an inverted/OLED look, or any other color for a custom palette. Changing this needs an app restart to recolor the icons.",
        "defaultValue": "0x000000",
        "sunlight": false
      },
      {
        "type": "select",
        "messageKey": "DateFormat",
        "label": "Date format",
        "description": "Format for the date display next to the battery. The time format (12h or 24h) follows your watch's own Time settings.",
        "defaultValue": "0",
        "options": [
          { "label": "European (Mo 21.05)", "value": "0" },
          { "label": "American (Mon 5/21)", "value": "1" },
          { "label": "ISO-style (Mo 05-21)", "value": "2" }
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
]