{
  Exports one selected NPC record as an NPC Visual preset.
  Output: Data\Viny Mods\NPC Visual\Presets
}
unit ExportNPCVisualPreset;

var
  sPresetName: string;
  bExported: boolean;

function FloatToStrJSON(f: Extended): string;
var
  s: string;
begin
  s := FloatToStr(f);
  s := StringReplace(s, ',', '.', [rfReplaceAll]);
  Result := s;
end;

function BoolToStrJSON(b: boolean): string;
begin
  if b then Result := 'true' else Result := 'false';
end;

function JSONEscape(s: string): string;
begin
  Result := s;
  Result := StringReplace(Result, '\', '\\', [rfReplaceAll]);
  Result := StringReplace(Result, '"', '\"', [rfReplaceAll]);
end;

function GetNormalizedFormID(eLink: IInterface): string;
var
  baseRec, f: IInterface;
  fullID, localID: Cardinal;
begin
  Result := '';
  if not Assigned(eLink) then Exit;

  baseRec := MasterOrSelf(eLink);
  f := GetFile(baseRec);
  fullID := FormID(baseRec);

  if ((fullID shr 24) = $FE) then
    localID := fullID and $00000FFF
  else
    localID := fullID and $00FFFFFF;

  Result := GetFileName(f) + '|' + IntToHex(localID, 1);
end;

function GetFormRefJSON(eLink: IInterface): string;
var
  baseRec: IInterface;
  edid, formID: string;
begin
  Result := '""';
  if not Assigned(eLink) then Exit;

  baseRec := MasterOrSelf(eLink);
  edid := EditorID(baseRec);
  formID := GetNormalizedFormID(baseRec);

  Result := '{ "editorID": "' + JSONEscape(edid) + '", "formID": "' + JSONEscape(formID) + '" }';
end;

function ProcessHeadParts(e: IInterface): string;
var
  parts, link: IInterface;
  i: integer;
  sl: TStringList;
begin
  parts := ElementByPath(e, 'Head Parts');
  if not Assigned(parts) then begin
    Result := '[]';
    Exit;
  end;

  sl := TStringList.Create;
  try
    for i := 0 to ElementCount(parts) - 1 do begin
      link := LinksTo(ElementByIndex(parts, i));
      if Assigned(link) then
        sl.Add(GetFormRefJSON(link));
    end;

    Result := '[';
    for i := 0 to sl.Count - 1 do begin
      if i > 0 then Result := Result + ', ';
      Result := Result + sl[i];
    end;
    Result := Result + ']';
  finally
    sl.Free;
  end;
end;

function ProcessTintLayers(e: IInterface): string;
var
  tints, tintItem: IInterface;
  i, tIndex, tPreset: integer;
  tR, tG, tB, tA: integer;
  tInterp: Extended;
  sLine: string;
  sl: TStringList;
begin
  tints := ElementByPath(e, 'Tint Layers');
  if not Assigned(tints) then begin
    Result := '[]';
    Exit;
  end;

  sl := TStringList.Create;
  try
    for i := 0 to ElementCount(tints) - 1 do begin
      tintItem := ElementByIndex(tints, i);

      tIndex := 0;
      if ElementExists(tintItem, 'TINI') then tIndex := GetElementNativeValues(tintItem, 'TINI');

      tInterp := 0.0;
      if ElementExists(tintItem, 'TINV') then tInterp := GetElementNativeValues(tintItem, 'TINV');

      tPreset := 0;
      if ElementExists(tintItem, 'TIRS') then tPreset := GetElementNativeValues(tintItem, 'TIRS');

      tR := 0; tG := 0; tB := 0; tA := 255;
      if ElementExists(tintItem, 'TINC\Red') then tR := GetElementNativeValues(tintItem, 'TINC\Red');
      if ElementExists(tintItem, 'TINC\Green') then tG := GetElementNativeValues(tintItem, 'TINC\Green');
      if ElementExists(tintItem, 'TINC\Blue') then tB := GetElementNativeValues(tintItem, 'TINC\Blue');
      if ElementExists(tintItem, 'TINC\Alpha') then tA := GetElementNativeValues(tintItem, 'TINC\Alpha');

      sLine := '{ "index": ' + IntToStr(tIndex) + ', ' +
               '"color": { "r": ' + IntToStr(tR) + ', "g": ' + IntToStr(tG) + ', "b": ' + IntToStr(tB) + ', "a": ' + IntToStr(tA) + ' }, ' +
               '"interpolation": ' + FloatToStrJSON(tInterp) + ', ' +
               '"preset": ' + IntToStr(tPreset) + ' }';
      sl.Add(sLine);
    end;

    Result := '[' + #13#10;
    for i := 0 to sl.Count - 1 do begin
      Result := Result + '    ' + sl[i];
      if i < sl.Count - 1 then Result := Result + ',';
      Result := Result + #13#10;
    end;
    Result := Result + '  ]';
  finally
    sl.Free;
  end;
end;

function ProcessMorphs(e: IInterface): string;
var
  morphs: IInterface;
  i: integer;
  v: Extended;
begin
  morphs := ElementByPath(e, 'NAM9');
  if not Assigned(morphs) then begin
    Result := '[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]';
    Exit;
  end;

  Result := '[';
  for i := 0 to ElementCount(morphs) - 1 do begin
    if i > 0 then Result := Result + ', ';
    v := GetNativeValue(ElementByIndex(morphs, i));
    Result := Result + FloatToStrJSON(v);
  end;
  Result := Result + ']';
end;

function Initialize: integer;
begin
  bExported := false;

  if not InputQuery('Export as Preset', 'Preset Name:', sPresetName) then begin
    Result := 1;
    Exit;
  end;

  sPresetName := Trim(sPresetName);
  if sPresetName = '' then begin
    AddMessage('Preset name cannot be empty. Operation cancelled.');
    Result := 1;
    Exit;
  end;
end;

function Process(e: IInterface): integer;
var
  flags: integer;
  json: TStringList;
  height, weight: Extended;
  isFemale, oppAnim: boolean;
  bodyR, bodyG, bodyB, bodyA: integer;
  rnam, wnam, doft, soft, vtck, hclf: IInterface;
  saveDir, filename: string;
begin
  if bExported or (Signature(e) <> 'NPC_') then Exit;

  json := TStringList.Create;
  try
    json.Add('{');

    height := 1.0;
    if ElementExists(e, 'DATA\Height') then
      height := GetElementNativeValues(e, 'DATA\Height');
    json.Add('  "height": ' + FloatToStrJSON(height) + ',');

    weight := 50.0;
    if ElementExists(e, 'DATA\Weight') then
      weight := GetElementNativeValues(e, 'DATA\Weight');
    json.Add('  "weight": ' + FloatToStrJSON(weight) + ',');

    flags := 0;
    if ElementExists(e, 'ACBS\Flags') then
      flags := GetElementNativeValues(e, 'ACBS\Flags');

    isFemale := (flags and 1) <> 0;
    oppAnim := (flags and $80000) <> 0;
    json.Add('  "isFemale": ' + BoolToStrJSON(isFemale) + ',');
    json.Add('  "oppositeGenderAnim": ' + BoolToStrJSON(oppAnim) + ',');

    bodyR := 255; bodyG := 255; bodyB := 255; bodyA := 255;
    if ElementExists(e, 'QNAM') then begin
      if ElementExists(e, 'QNAM\Red') then bodyR := GetElementNativeValues(e, 'QNAM\Red');
      if ElementExists(e, 'QNAM\Green') then bodyG := GetElementNativeValues(e, 'QNAM\Green');
      if ElementExists(e, 'QNAM\Blue') then bodyB := GetElementNativeValues(e, 'QNAM\Blue');
      if ElementExists(e, 'QNAM\Alpha') then bodyA := GetElementNativeValues(e, 'QNAM\Alpha');
    end;
    json.Add('  "bodyTintColor": { "r": ' + IntToStr(bodyR) + ', "g": ' + IntToStr(bodyG) + ', "b": ' + IntToStr(bodyB) + ', "a": ' + IntToStr(bodyA) + ' },');

    rnam := LinksTo(ElementBySignature(e, 'RNAM'));
    if Assigned(rnam) then json.Add('  "race": ' + GetFormRefJSON(rnam) + ',');

    wnam := LinksTo(ElementBySignature(e, 'WNAM'));
    if Assigned(wnam) then json.Add('  "skin": ' + GetFormRefJSON(wnam) + ',');

    doft := LinksTo(ElementBySignature(e, 'DOFT'));
    if Assigned(doft) then json.Add('  "defaultOutfit": ' + GetFormRefJSON(doft) + ',');

    soft := LinksTo(ElementBySignature(e, 'SOFT'));
    if Assigned(soft) then json.Add('  "sleepOutfit": ' + GetFormRefJSON(soft) + ',');

    vtck := LinksTo(ElementBySignature(e, 'VTCK'));
    if Assigned(vtck) then json.Add('  "voice": ' + GetFormRefJSON(vtck) + ',');

    hclf := LinksTo(ElementBySignature(e, 'HCLF'));
    if Assigned(hclf) then json.Add('  "hairColor": ' + GetFormRefJSON(hclf) + ',');

    json.Add('  "headParts": ' + ProcessHeadParts(e) + ',');
    json.Add('  "tintLayers": ' + ProcessTintLayers(e) + ',');
    json.Add('  "faceMorphs": ' + ProcessMorphs(e));

    json.Add('}');

    saveDir := DataPath + 'Viny Mods\NPC Visual\Presets\';
    ForceDirectories(saveDir);
    filename := saveDir + sPresetName + '.json';

    json.SaveToFile(filename);
    AddMessage('[NPC Visual] Preset created -> ' + filename);

    bExported := true;
  finally
    json.Free;
  end;
end;

end.
