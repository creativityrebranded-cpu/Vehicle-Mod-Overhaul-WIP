#pragma once
// Existing and additive mod-owned editor actions. Shared by native UI and layout tests.
		enum Id {
			kNew = 1, kSave, kCancel, kDeleteRoute, kPrevRoute, kNextRoute, kSelect, kDrop, kDrag, kLanding, kSpawn,
			kZUp, kZDown, kReverse, kDeletePoint, kLoop, kUndo, kRedo, kWays, kSpeedDown, kSpeedUp, kVehDown, kVehUp,
			kSchedule, kStartDown, kStartUp, kEndDown, kEndUp, kTest, kTimeDown, kTimeUp, kTimeLive, kClose, kConnect, kShareDown, kShareUp,
			kEditOriginal, kRadiusDown, kRadiusUp, kBake, kMoveLeft, kMoveRight, kMoveForward, kMoveBack, kStartSwap, kRoundPoint, kRoundDown, kRoundUp, kRename, kColor,
			// Stage 2 (2026-10-30): the Route tool, the toolbar menus, the folded groups and the panel's two-way choices.
			kRouteTool, kMoreMenu, kViewMenu, kCopy, kMirror, kGroupFlow, kGroupPosition, kGroupRules, kProblems,
			kRouteUp, kRouteDown, kTurnLeft, kTurnRight, kOneWay, kTwoWay, kLoopOn, kLoopOff, kAllDay, kByHours, kSharp, kRounded,
			// Traffic Manager V1 (2026-10-30): vehicle rules.
			kVehicleRules, kAllowAll, kSheetBack, kTypesMore, kSplit, kJoin, kRestore,
			kJunctionBoth, kJunctionExit, kJunctionMerge, kJunctionCross, kJunctionSuggest,
			kShapeTool, kZoneTool, kTimeRules, kByWords, kWordsAdd, kBlendQuick, kBlendGentle, kBlendSlow, kRoleNext,
			kCity, kCityOn, kCityOff, kDensityDown, kDensityUp, kLimitDown, kLimitUp, kOriginalsOn, kOriginalsOff,
			kPlaceDone, kLayoutSingle, kLayoutRow, kLayoutBays, kLayoutRing, kSpacesDown, kSpacesUp, kAreaTurnDown, kAreaTurnUp,
			kPurposeTaxi, kPurposeDepot, kPurposeStop, kStayShort, kStayLong, kStayNeeded, kFullQueue, kFullCircle, kFullOn, kWhoParks,
			kZoneKeep, kZoneSlow, kZoneSizeDown, kZoneSizeUp, kZoneTopDown, kZoneTopUp, kZoneSpeedDown, kZoneSpeedUp,
			kGroupNext, kGroupSheet, kGroupNew, kGroupRename, kGroupDelete,
			kSizeDown, kSizeUp, kShapeADown, kShapeAUp, kShapeBDown, kShapeBUp, kShapeCDown, kShapeCUp,
			kReadme, kTravelNext, kSectionTravelNext, kRoadRuleNext, kCorridorDown, kCorridorUp, kSectionSpeedDown, kSectionSpeedUp, kPriorityDown, kPriorityUp, kSignalPhaseNext,
            kLaneDesign,kLaneRules,kLaneDimensions,kLaneScope,kLaneFocus,kLanePreset,kLaneBack,kLaneReset,kLaneVolumes,
            kLaneTravelRules,kLaneHeightRules,
            kInspectMovement,kInspectNext,kJourneySheet,kJourneyEnable,kJourneyLoop,kJourneyAdd,kJourneyNext,kJourneyRemove,kJourneyDwellDown,kJourneyDwellUp,kJourneyEarlier,kJourneyLater,kJourneyActions,kJourneyWait,kJourneyTrigger,kJourneyTimeout,kJourneyFallback,kJourneySpeedDown,kJourneySpeedUp,kJourneyMode,kJourneyRelease,kRestrictionSheet,kRestrictionCity,kRestrictionGroup,kRestrictionModes,kRestrictionClosure,kRestrictionSpeedDown,kRestrictionSpeedUp,kRestrictionFloor,kRestrictionCeiling,kRestrictionWindow,kRestrictionEnd,kRestrictionNoChange,kRestrictionBack,kRestrictionZone,kRestrictionZoneFloor,kRestrictionZoneTop,kPopulationSheet,kPopulationNext,kPlaceAccess,kAccessEntry,kAccessExit,kAccessEntryRoute,kAccessExitRoute,kAccessEntryLane,kAccessExitLane,kAccessUndoEntry,kAccessUndoExit,kAccessHeightDown,kAccessHeightUp,kAccessWidthDown,kAccessWidthUp,kAccessClearanceDown,kAccessClearanceUp,kAccessDone,kLanePlacement,kLaneMoveMap,kLanePlaceApply,kLaneEnvelope,kEnvelopeWidthDown,kEnvelopeWidthUp,kEnvelopeLengthDown,kEnvelopeLengthUp,kEnvelopeHeightDown,kEnvelopeHeightUp,kWorldDefaultsFromRoute,kGroupDefaultsFromRoute,kWorldDefaultsClear,kGroupDefaultsClear,kLaneUseDefaults,kNetworkPreview,kPreviewDestination,kPreviewTrace,kPreviewSignalNext,kPreviewLivePaths,
            kLinkLaneFromDown,kLinkLaneFromUp,kLinkLaneToDown,kLinkLaneToUp,
            kKitOpen=8000,kKitEnable,kKitProfiles,kKitPath,kKitAnchorNext,kKitLaneNext,kKitSide,kKitStep,kKitPlace,kKitDetach,kKitAuto,kKitQuality,kKitSpacing,kKitLink,kKitProfileAdd,kKitProfileNext,kKitProfileRemove,kKitProfileShape,kKitProfileScope,kKitVertexNext,kKitVertexAdd,kKitVertexRemove,
            kKitWholeBase=8100,kKitCoordBase=8120,kKitProfileBase=8140,kKitLast=8160,kPermanentSave=8170,kPermanentDelete,kPanelPrevious=8180,kPanelNext,kVehicleScope,kLaneVehicleRules,
            kLaneEditBase=7000, // two actions per laneplan field: decrease/increase
			kDayRow = 5000,    // + 4 x a zone of the day: earlier, later, next word, remove
			kLevelBase = 5500, // + 2 x a word: less, more
			kPlaceRow = 5600,  // + a line of the places list (landing areas, then zones)
			kShapeBase = 5900, // + a shape; the last is parallel lanes
			kGroupRow = 6000,  // + 0 all routes, + 1 + a group (route groups sheet)
			kProblemShow = 4000, kProblemFix = 4500,  // + a problem's place in the list
			kTypeBase = 3000,  // + a vehicle type in the vehicle rules sheet
			kRowBase = 1000,   // + index in the routes list (editor routes, then the other files' routes)
			kViewBase = 2000,  // + View item
		};
// Hazard boxes (2026-11-21): the Hazards tool and its steppers.
enum HazardId { kHazardTool=9800,kHazardDone,kHazardDelete,kHazardAcrossDown,kHazardAcrossUp,kHazardAlongDown,kHazardAlongUp,kHazardHeightDown,kHazardHeightUp,kHazardBaseDown,kHazardBaseUp,kHazardTurnLeft,kHazardTurnRight,
    kHazardInherit,kHazardKeep,kHazardDismiss,kHazardKeepAll,kHazardPlace,kHazardNextSuggest };
// Three-layer F7 (2026-11-03): group tabs, shape and link-shape choices, dock values, tag controls, Field panel rows.
enum F7Id { kF7GroupBase=9600,kF7PrimBase=9610,kF7LinkBase=9620,kF7ValueBase=9630,kF7TagPrev=9640,kF7TagNext,kF7TagMinus,kF7TagPlus,kF7TagToggle,kF7TagTravel,
    kF7FlowAlong=9650,kF7FlowAgainst,kF7PosDown,kF7PosUp,kF7TravelBase=9660,kF7FieldShape=9670,kF7FieldWidth,kF7FieldParam,kF7FieldPrev,kF7FieldNext,kF7FieldMedian=9675,kF7Taper=9513,kF7Arrange=9680,kF7NodeView=9681,kRoadSnap=9690,kRoadFit=9691 };
enum ContextId { kContextBrowse=9000,kContextMore,kContextBack,kContextPin,kContextWidth,kContextHeight,kContextTravel,kContextVehicles,kContextMovement,kContextScope,kContextMove,kContextTurn,kContextZ,kContextExact,kContextCorner=9018,kContextSharp,kContextRound,kContextCornerDrag,kContextCategories=9100,kContextNumber=9200 };
