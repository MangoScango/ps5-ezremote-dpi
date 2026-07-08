# ps5-ezremote-dpi

A payload that runs in the background to receive package install request similar to etaHEN DPI. Only supports http/https URLs. This is a standalone payload that doesn't need etaHEN or kstuff. All it does is pass the URL to the pkg installer, and the native PS5 pkg download/install process does the rest. As a result, you are free to install packages in the background while playing a game or while in rest mode. Tested on 12.40, but should work up through 13.42.

## Instructions
 - Run your favorite kernel exploit and start an elfloader
 - Send ezremote-dpi.elf to elfloader
 - Wait for the message "ezRemote DPI listening on port 9040" to appear
 - Send URL to install from Linux
   ```bash
   echo 'https://example.org/game.pkg' | nc PS5_IP 9040
   ```
 - On Windows use ncat instead
     ```bash
     echo 'https://example.org/game.pkg' | ncat PS5_IP 9040
     ```

## Optional Metadata Parameters

You can optionally provide metadata about the package being installed via URL parameters. This doesn't change anything functionally, but will allow the console UI to properly render the game information during install.
  - Example:
     ```bash
     echo 'https://example.org/game.pkg?content_id=UP0700-CUSA08692_00-DARKSOULSHD00000&name=Dark Souls Remastered&icon=/icons/UP0700-CUSA08692_00-DARKSOULSHD00000.png' | ncat PS5_IP 9040
     ```
## CDN Manifest

You can push split packages straight from the CDN if you have the manifest. Useful for redownloading content you already have a license to. Obviously, this requires unblocking the CDN in your DNS provider, so use with caution to avoid accidental updates.
  - Example, redownload Astro's Playroom:
     ```bash
     echo 'https://sgst.prod.dl.playstation.net/sgst/prod/00/PPSA01325_00/app/info/51/f_0339a29780d866561cc22382c5b9f1ad8bd19c0960f499c5b637f0d833879457/IP9100-PPSA01325_00-PREINMASTER00000.json' | ncat PS5_IP 9040
     ```