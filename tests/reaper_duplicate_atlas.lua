local project = 0
local atlas_track = nil

for index = 0, reaper.CountTracks(project) - 1 do
  local track = reaper.GetTrack(project, index)
  local _, name = reaper.GetTrackName(track)
  if name == "Atlas" then
    atlas_track = track
    break
  end
end

if atlas_track == nil then
  reaper.ShowConsoleMsg("Atlas duplication stress: no Atlas track found\n")
  return
end

reaper.SetOnlyTrackSelected(atlas_track)
reaper.Main_OnCommand(40062, 0) -- Track: Duplicate tracks
reaper.Main_SaveProject(project, false)
reaper.ShowConsoleMsg("Atlas duplication stress: duplicate action completed\n")
