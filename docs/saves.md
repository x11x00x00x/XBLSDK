# Game Saves (native Xbox format)

The Test Game stores its progress using the **same on-disk format retail original
Xbox games use**, so saves appear in the Xbox dashboard's memory/save manager and
are picked up by save backup/restore tools. This is implemented in the SDK
(`sdk/xbl_save.c`) and exposed through `xblsdk.h`.

## The format

Retail Xbox saves live on the hard drive under the **UDATA** tree:

```
E:\UDATA\<TitleID>\                     per-title folder (8 hex digits)
  TitleMeta.xbx                         TitleName=<game name>
  <SaveFolder>\                         one folder per save slot
    SaveMeta.xbx                        Name=<save label>
    data.bin                            the game's own save payload (any bytes)
```

- `TitleMeta.xbx` and `SaveMeta.xbx` are small **INI-style text** files
  (`key=value` lines). The dashboard reads `TitleName` and `Name` from them to
  label the title and each save.
- `<TitleID>` is the running XBE's certificate Title ID, read at runtime from
  `CURRENT_XBE_HEADER->CertificateHeader->TitleID`. For stock-`cxbe` homebrew
  this is `FFFF0002`, so the Test Game's saves live under `E:\UDATA\FFFF0002\`.
- Everything beside `SaveMeta.xbx` is yours. The SDK writes a single `data.bin`
  blob; the Test Game stores a tiny INI payload in it (`best=`, `runs=`, `map=`).

> Reference: [xboxdevwiki – Xbox Savegame System](https://xboxdevwiki.net/Xbox_Savegame_System).
> The SDK uses only Win32 file APIs (`CreateDirectory`/`CreateFile`/`WriteFile`/
> `ReadFile`/`FindFirstFile`) that nxdk provides — there is no `XCreateSaveGame`
> in nxdk, so the layout is written directly.

## Requirements

The `E:` drive (HDD partition 1) must be mounted before using saves. The Test
Game already does this at startup:

```c
nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
```

## SDK surface

See [sdk-reference.md](sdk-reference.md) for full signatures.

```c
// Once, after mounting E: — ensures E:\UDATA\<TitleID>\ + writes TitleMeta.xbx.
xbl_save_init("TestGame");

// Write a save slot (folder name) with a display label + payload bytes.
const char *blob = "best=1820\r\nruns=37\r\nmap=3\r\n";
xbl_save_write("PROFILE", "TestGame Profile", blob, strlen(blob));

// Read it back.
char buf[256]; size_t len = 0;
xbl_save_read("PROFILE", buf, sizeof(buf), &len);

// Enumerate saves (slot folder + display name from SaveMeta.xbx).
XblSaveInfo saves[XBL_SAVE_LIST_MAX]; int n = 0;
xbl_save_list(saves, XBL_SAVE_LIST_MAX, &n);

// Delete a save slot (SaveMeta.xbx + data.bin + folder).
xbl_save_delete("PROFILE");
```

Slot names are validated to `[A-Za-z0-9_]`, 1–15 chars (they become folder
names). `data.bin` may hold any bytes (text or binary); the Test Game uses text
for readability.

## In the Test Game

- At boot the game calls `xbl_save_init("TestGame")` and loads the `PROFILE` slot
  (best score, run count, last selected map).
- The profile is saved automatically after a verified run and when leaving the
  game, and manually from the **Save / Load** menu.
- The **Save / Load** screen lists every save under the title (read from
  `SaveMeta.xbx`), can write the current profile, and can load any listed save.

Because this is the real dashboard format, the resulting `E:\UDATA\FFFF0002\`
tree shows up in the Xbox memory manager and in tools like the sibling
`originalxboxgamesaves` backup utility.

## Notes & limitations

- **Shared Title ID.** All stock-`cxbe` homebrew share Title ID `FFFF0002` unless
  the XBE certificate is patched, so different homebrew titles can collide under
  the same UDATA folder. For a real release you would patch the Title ID.
- **No thumbnails.** The dashboard can also show `TitleImage.xbx` / `SaveImage.xbx`
  icons; the SDK writes only the required text meta files. Add those yourself if
  you want custom save icons.
- **Single payload file.** The SDK models a save as one `data.bin`. If you need
  multiple files per save, write them next to `SaveMeta.xbx` with your own
  `CreateFile` calls using the same folder.
