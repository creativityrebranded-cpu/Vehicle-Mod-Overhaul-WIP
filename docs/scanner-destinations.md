# Named scanner destinations

When the scanner highlights an outpost or another map marker, its displayed location name identifies the aimed destination. The name appears beside the scanner target distance, in the marker confirmation, and stays attached to queued stops.

The implementation reads the active `RightInfoDisplay_mc` MapMarker panel and its `MapMarkerInfo_mc.description_mc.description_tf` first line on the Scaleform movie thread. The nearby-area name is separate and does not select a destination. Snapshot data expires after 200 ms and clears on menu destruction or save load.

Captured planet map records include their copied names. Matching uses name, viewing direction, and the scanner range to distinguish places with repeated names. A selected named destination takes priority over a nearer collision ray. Ordinary aiming still respects foreground hits.

If planet map coordinates are unavailable (for example, before opening the star map), the game scanner range projects an estimated position along the camera view. Estimated positions retain the `~` indicator, including after marker placement. This projection is not an exact reference position.

Validation: release DLL build and six offline destination-selection cases. Live scanner display and travel still require an in-game check.
