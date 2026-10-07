param([string]$PortA='COM13',[string]$PortB='COM16')
$ErrorActionPreference='Stop'
$a=[System.IO.Ports.SerialPort]::new($PortA,115200)
$b=[System.IO.Ports.SerialPort]::new($PortB,115200)
$rng=[Random]::new(154)
function Receive-Bytes($port,$count) {
  $buffer=[byte[]]::new($count)
  $offset=0
  while($offset -lt $count) { $offset += $port.Read($buffer,$offset,$count-$offset) }
  return ,$buffer
}
try {
  foreach($port in @($a,$b)) { $port.ReadTimeout=5000; $port.WriteTimeout=5000; $port.Open(); $port.DiscardInBuffer() }
  foreach($size in @(1,63,64,65,95,96,97,511,512,513,1024)) {
    $sa=[byte[]]::new($size); $sb=[byte[]]::new($size)
    $rng.NextBytes($sa); $rng.NextBytes($sb)
    $a.Write($sa,0,$size)
    $rb=Receive-Bytes $b $size; $b.Write($sb,0,$size); $ra=Receive-Bytes $a $size
    if([Convert]::ToBase64String($ra) -ne [Convert]::ToBase64String($sb) -or [Convert]::ToBase64String($rb) -ne [Convert]::ToBase64String($sa)) { throw "Data mismatch at $size bytes" }
    "PASS: $size binary bytes each direction"
  }
} finally { $a.Dispose(); $b.Dispose() }

