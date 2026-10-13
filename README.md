SKSE_UAPNG
Utlimate Animated Potion NG - SKSE Plugin

Ground up Re-write of Ultimate Animated Potion

Nexus Mods https://www.nexusmods.com/skyrimspecialedition/mods/97674

To skip UAPNG's animation for Skooma, add this to `SKSE/Plugins/UAPNG.ini`:

```ini
[BlacklistedPotions]
0x57A7A~Skyrim.esm
```

Each line identifies one item. `[BlacklistedPotions]` skips its animation; `[AllowedPotions]` enables the animation for an alchemy item that UAPNG would otherwise not recognize. The blacklist takes priority when an item appears in both sections.

Both sections use QTR Utils' FormReader and accept these formats:

1. EditorID: `Skooma`, the record's internal identifier in the plugin. This requires powerofthree's Tweaks with `Load EditorIDs` enabled.
2. Full hexadecimal FormID: `00057A7A`, `0057A7A`, or `0x57A7A`. Without the `0x` prefix, use seven or eight hexadecimal digits. Full FormIDs include the current load-order index.
3. Plugin-relative FormID: `57A7A~Skyrim.esm` or `0x57A7A~Skyrim.esm`. The hexadecimal number identifies the record within the named plugin, independently of its load-order index. Preserve spaces in plugin filenames, for example `0x800~My Potions.esp`.

Numeric formats work without powerofthree's Tweaks. Put one entry per line, without `key=value`. Use `;` for comments. Empty, unresolved, or unsuitable records are ignored. Restart the game after changing these lists.

Build with [CommonLibVR-MIT](https://github.com/QTR-Modding/CommonLibVR-MIT/tree/b72705a607b82c7baa8d977e90303064bf3b5719) for Skyrim SE, AE, GOG, and VR.
Set `COMMONLIB_SSE_FOLDER` to that checkout before configuring the project.
