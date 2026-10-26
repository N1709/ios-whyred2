# display

- **Hardware:** `AppleCLCD` MDP/DSI path, plus the framebuffer that ABL leaves
  live at `0x9d400000` (`1080x2160`, 32-bit `a8r8g8b8`, stride 4320 bytes).
- **Code:** `sweetfb`/`AppleCLCD` glue in XNU, reached through
  `xnu/patches/0046-sweet-iosurface-iomfb.patch`,
  `0085-topaz-fb-stayon.patch`, `0092-topaz-direct-mdp-scanout.patch`,
  `0096-topaz-smooth-fade.patch`, `0099-...console-batch.patch`,
  `0103-topaz-copy-path-back-buffer.patch`, `0105-topaz-applearmbacklight.patch`,
  `0110-topaz-backlight-optin.patch` and `0127-topaz-sweetbusy.patch`.
- **Proven on the phone:** kernel console renders into the ABL framebuffer at
  1080x2160; the boot log is readable on the panel.
- **Not proven:** `IOMobileFramebuffer` open by `backboardd` (it never gets
  that far yet), hardware composition, backlight control.
- **Next:** nothing until the code-signing/shared-cache faults are cleared;
  then check which `IOMobileFramebuffer` properties `backboardd` needs.

Mirrors: `SweetFramebuffer.cpp` and `SweetSurface.cpp` (the code this kernel
actually runs, regenerated from the patches).

Borrowed reference: `hakios/MiniIOMobileFramebuffer.cpp` from HakiOS. It solves
the same problem (present `IOSurface` layers into the bootloader's framebuffer,
fake vsync with a timer) with a different implementation; compare the property
sets `backboardd` asks for before changing our `AppleCLCD` path.
