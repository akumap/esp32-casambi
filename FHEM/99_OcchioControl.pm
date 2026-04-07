package main;
use JSON;

my $_updatingStatus = 0;
my %_debouncePending;

sub OcchioControl_Initialize {
  my $hash = shift;
}

sub OcchioControl_Notify {
  my ($device, $event) = @_;

  return if $_updatingStatus;

  Log 1, "OcchioControl: Device=$device Event='$event'";

  # "undefined XX" Events von Homebridge behandeln
  if($event =~ /^undefined\s+(.*)/) {
    my $value = int($1);
    if($value <= 100) {
      $event = "brightness $value";
      Log 1, "OcchioControl: Converted to Event='$event' (Brightness)";
    } else {
      # Gerätespezifische Grenzen aus Attributen, Fallback auf Casambi-typische Werte
      my $cctMin = AttrVal($device, "cctMin", 2700);
      my $cctMax = AttrVal($device, "cctMax", 4000);
      my $kelvin = int(1000000 / $value);
      $kelvin = $cctMin if $kelvin < $cctMin;
      $kelvin = $cctMax if $kelvin > $cctMax;
      $event = "colorTemp $kelvin";
      Log 1, "OcchioControl: Converted to Event='$event' (ColorTemp from $value Mired)";
    }
  }

  my $unit_id = AttrVal($device, "casambiUnitId", "");
  return if(!$unit_id);

  # on/off sofort senden
  if($event =~ /^(on|off)$/) {
    OcchioControl_SendCommand($device, $unit_id, $event);
    return;
  }

  # Analoge Typen: debounced senden
  my $type;
  if    ($event =~ /^brightness\s+/) { $type = "brightness"; }
  elsif ($event =~ /^colorTemp\s+/)  { $type = "colorTemp";  }
  elsif ($event =~ /^vertical\s+/)   { $type = "vertical";   }
  else  { return; }

  my $key = "${device}_${type}";
  $_debouncePending{$key} = { device => $device, unit_id => $unit_id, event => $event };

  RemoveInternalTimer("debounce_${key}");
  InternalTimer(
    gettimeofday() + 0.3,
    sub {
      my $pending = delete $_debouncePending{$key};
      return unless $pending;
      OcchioControl_SendCommand($pending->{device}, $pending->{unit_id}, $pending->{event});
    },
    "debounce_${key}"
  );
}

sub OcchioControl_SendCommand {
  my ($device, $unit_id, $event) = @_;
  my $esp32_ip = "192.168.178.111";
  my ($url, $json);

  if($event eq "on") {
    $url  = "http://$esp32_ip/api/units/$unit_id/level";
    $json = '{"level":255}';
    fhem("setreading $device state on");
    # fhem("setreading $device brightness 100");
  }
  elsif($event eq "off") {
    $url  = "http://$esp32_ip/api/units/$unit_id/level";
    $json = '{"level":0}';
    fhem("setreading $device state off");
    # fhem("setreading $device brightness 0");
  }
  elsif($event =~ /^brightness\s+(.*)/) {
    my $brightness = int($1);
    my $level = int($brightness * 2.55);
    $url  = "http://$esp32_ip/api/units/$unit_id/level";
    $json = '{"level":'.$level.'}';
    fhem("setreading $device brightness $brightness");
    # fhem("setreading $device state " . ($brightness > 0 ? "on" : "off"));
  }
  elsif($event =~ /^colorTemp\s+(.*)/) {
    my $value = int($1);
    my $kelvin = ($value < 500) ? int(1000000 / $value) : $value;

    # Gerätespezifische Grenzen aus Attributen, Fallback auf Casambi-typische Werte
    my $cctMin = AttrVal($device, "cctMin", 2700);
    my $cctMax = AttrVal($device, "cctMax", 4000);
    $kelvin = $cctMin if $kelvin < $cctMin;
    $kelvin = $cctMax if $kelvin > $cctMax;

    my $mired = int(1000000 / $kelvin);
    $url  = "http://$esp32_ip/api/units/$unit_id/temperature";
    $json = '{"kelvin":'.$kelvin.'}';
    fhem("setreading $device colorTemp $mired");
  }
  elsif($event =~ /^vertical\s+(.*)/) {
    my $vertical = int($1);
    $vertical = 0   if $vertical < 0;
    $vertical = 255 if $vertical > 255;
    $url  = "http://$esp32_ip/api/units/$unit_id/vertical";
    $json = '{"vertical":'.$vertical.'}';
    fhem("setreading $device vertical $vertical");
  }
  else {
    return;
  }

  Log3 $device, 3, "$device: Casambi -> Unit $unit_id: $event";

  HttpUtils_NonblockingGet({
    url     => $url,
    timeout => 5,
    method  => "POST",
    header  => "Content-Type: application/json",
    data    => $json,
    callback => sub {
      my ($param, $err, $data) = @_;
      Log3 $device, 2, "$device: Casambi error: $err" if $err;
    }
  });
}

sub OcchioControl_UpdateStatus {
  my $esp32_ip = "192.168.178.111";

  HttpUtils_NonblockingGet({
    url     => "http://$esp32_ip/api/units",
    timeout => 5,
    callback => sub {
      my ($param, $err, $data) = @_;
      if($err) {
        Log 2, "OcchioControl: UpdateStatus HTTP error: $err";
        return;
      }

      my $decoded;
      eval { $decoded = decode_json($data); };
      if($@ || !$decoded) {
        Log 2, "OcchioControl: UpdateStatus JSON error: $@";
        return;
      }

      my %deviceMap = (
        1  => "Occhio_AZ_Stehleuchte",
        2  => "Occhio_Piu_Spuele",
        3  => "Occhio_Kueche",
        5  => "Occhio_Peter_Nachttisch",
        7  => "Occhio_Mito_Sospeso",
        8  => "Occhio_Mel_Luna",
        9  => "Occhio_AZ_Decke",
        10 => "Occhio_AZ_Schrank",
        11 => "Occhio_IFC",
      );

      foreach my $unit (@{$decoded->{units}}) {
        my $devName = $deviceMap{$unit->{id}};
        next unless $devName;

        my $dev_hash = $defs{$devName};
        if(!$dev_hash) {
          Log 2, "OcchioControl: Device '$devName' nicht in FHEM gefunden!";
          next;
        }

        my $online     = $unit->{online} ? "true" : "false";
        my $on         = $unit->{on}     ? "on"   : "off";
        my $level      = $unit->{level}  // 0;
        my $brightness = int($level / 2.55 + 0.5);
        $brightness    = 100 if $brightness > 100;

        readingsBeginUpdate($dev_hash);
        readingsBulkUpdate($dev_hash, "online",     $online);
        readingsBulkUpdate($dev_hash, "state",      $on);
        readingsBulkUpdate($dev_hash, "brightness", $brightness);

        if(defined $unit->{vertical}) {
          readingsBulkUpdate($dev_hash, "vertical", $unit->{vertical});
        }

        if(defined $unit->{colorTemp} && defined $unit->{cctMin} && defined $unit->{cctMax}) {
            my $raw = $unit->{colorTemp};
            my $min = $unit->{cctMin};
            my $max = $unit->{cctMax};
            if($max > $min) {
                # raw=0 → cctMin, raw=255 → cctMax
                my $kelvin = int($min + ($raw / 255.0) * ($max - $min) + 0.5);
                my $mired  = int(1000000 / $kelvin);
                readingsBulkUpdate($dev_hash, "colorTemp", $kelvin);
            }
        }

        $_updatingStatus = 1;
        readingsEndUpdate($dev_hash, 1);
        $_updatingStatus = 0;

        Log 4, "OcchioControl: Updated $devName (state=$on, brightness=$brightness)";
      }
    }
  });
}

1;
