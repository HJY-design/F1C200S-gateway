'use strict';
let f407Parameters=null,f407Reading=false;
let f407BatteryProfile=null;
function estimateBatteryPercent(voltageMv,profile){
  if(!profile?.configured||!Number.isFinite(voltageMv)||voltageMv<=0||!Number.isInteger(profile.series_cells)||profile.series_cells<1||profile.series_cells>16)return null;
  if(!Array.isArray(profile.cell_voltage_soc)||profile.cell_voltage_soc.length<2)return null;
  const curve=profile.cell_voltage_soc.map(p=>Array.isArray(p)?p.slice(0,2):[]).sort((a,b)=>a[0]-b[0]);
  if(curve.some((p,i)=>p.length!==2||!p.every(Number.isFinite)||p[0]<=0||p[1]<0||p[1]>100||(i&&(p[0]<=curve[i-1][0]||p[1]<curve[i-1][1]))))return null;
  const cellMv=voltageMv/profile.series_cells;
  if(cellMv<=curve[0][0])return Math.round(curve[0][1]);
  for(let i=1;i<curve.length;i++)if(cellMv<=curve[i][0]){
    const a=curve[i-1],b=curve[i];return Math.round(a[1]+(b[1]-a[1])*(cellMv-a[0])/(b[0]-a[0]));
  }
  return Math.round(curve[curve.length-1][1]);
}
function f407BatteryText(data,online){
  if(!online||!data?.adc_valid||!Number.isFinite(data.battery_mv_estimate))return '估算电量 —';
  const percent=estimateBatteryPercent(data.battery_mv_estimate,f407BatteryProfile);
  if(percent===null)return '估算电量 待配置';
  const threshold=f407BatteryProfile?.charge_hint_percent;
  return `估算电量 ≈${percent}%${Number.isFinite(threshold)&&percent<=threshold?' · 建议充电':''}`;
}
function renderF407(info){
  if(!$('f407-online'))return;const data=info?.status?.data;
  $('f407-online').textContent=info?.online?'主控在线':'等待主控数据';
  $('f407-runtime').textContent=data?`${data.firmware} · ${(data.hclk_hz/1000000).toFixed(0)} MHz · ${data.hse_ready?'外部时钟':'内部时钟'} · ${data.armed?'输出运行中':'输出已停止'}`:'UART1 · 115200 8N1';
  $('f407-counts').textContent=data?`编码器：${data.encoder_count.join(' / ')} · 电池估算 ${(data.battery_mv_estimate/1000).toFixed(2)} V · ${f407BatteryText(data,info?.online)}`:'等待编码器数据';
  $('f407-counts').title=f407BatteryProfile?.configured?`${f407BatteryProfile.series_cells}S 锂电池 · ${f407BatteryProfile.capacity_mah} mAh · 标称 ${(f407BatteryProfile.nominal_mv/1000).toFixed(1)} V · 充满 ${(f407BatteryProfile.full_charge_mv/1000).toFixed(1)} V。电量按电压粗估，静置时更适合判断；带载、温度和电池状态会影响结果。`:'';
  const inertial=info?.inertial?.data,recording=state?.inertial_recording;
  if($('f407-inertial')){
    const lines=['惯导台架 · 每颗芯片自身坐标 · 相对航向 · 车辆位置尚未标定'];
    if(inertial)for(const [key,label]of[['bmi088','BMI088'],['mpu6050','MPU6050']]){const s=inertial[key];if(s)lines.push(`${label}：${s.valid?'姿态有效':s.zeroing?'正在静止标零':'等待有效姿态'} · 横滚 ${(s.rpy_mdeg[0]/1000).toFixed(2)}° · 俯仰 ${(s.rpy_mdeg[1]/1000).toFixed(2)}° · 标零后航向累计 ${(s.yaw_drift_mdeg/1000).toFixed(3)}°`);}
    else lines.push('点击“读取惯导”获取数据；后台记录期间自动更新缓存。');
    if(recording)lines.push(`后台记录：${recording.phase==='collecting'?'进行中':recording.phase==='completed'?'已结束':recording.phase} · ${duration(recording.elapsed_s)} / ${duration(recording.duration_s)} · 请求错误 ${recording.request_errors} · 异常记录 ${recording.invalid_records}`);
    $('f407-inertial').textContent=lines.join('\n');
  }
  const target=$('flash-target');if(target&&!target.dataset.configured&&state?.configured_target){target.value=state.configured_target;target.dataset.configured='1';}
}
async function readF407Parameters(){
  if(f407Reading)throw new Error('参数正在读取，请稍候');f407Reading=true;
  try{
    const result=await api('param-get');if(!result.ok||!result.parameters)throw new Error(result.error||result.code||'主控未返回参数');
    f407Parameters=result.parameters;$('f407-param-json').textContent=JSON.stringify(f407Parameters,null,2);
    $('f407-param-state').textContent=`已从主控读回 · ${result.saved?'与已保存参数一致':'包含未保存到 Flash 的修改'} · 版本 ${result.generation}`;
    const select=$('f407-param-key'),previous=select.value;select.replaceChildren();
    for(const key of Object.keys(f407Parameters)){const option=document.createElement('option');option.value=key;option.textContent=key;select.append(option);}
    if(previous&&previous in f407Parameters)select.value=previous;
    $('f407-param-value').value=f407Parameters[select.value]??'';return result;
  }catch(error){$('f407-param-state').textContent=error.message;throw error;}finally{f407Reading=false;}
}
async function applyF407Profile(profile){
  const entries=Object.entries(profile.values||{});if(!entries.length)throw new Error('参数组为空');
  await readF407Parameters();
  for(const [key,value]of entries)if(!(key in f407Parameters)||!Number.isInteger(value)||value< -2147483648||value>2147483647)throw new Error(`参数无效：${key}`);
  let written=0;
  try{
    for(const [key,value]of entries){notice(`正在写入 ${++written}/${entries.length}：${key}`);const reply=await api('param-set',{key,value},'POST');if(!reply.ok)throw new Error(reply.error||reply.code);}
    await readF407Parameters();for(const [key,value]of entries)if(f407Parameters[key]!==value)throw new Error(`读回值不一致：${key}`);
    notice('参数组已写入 F407 RAM，并完成读回确认。');
  }catch(error){await readF407Parameters().catch(()=>{});throw new Error(`参数组未全部确认，请以当前读回值为准：${error.message}`);}
}
document.addEventListener('DOMContentLoaded',()=>{
  fetch('battery-profile.json',{cache:'no-store'}).then(r=>r.ok?r.json():null).then(profile=>{f407BatteryProfile=profile;renderF407(state?.f407);}).catch(()=>{});
  const card=document.createElement('article');card.className='card';
  const title=document.createElement('div');title.className='card-title';const heading=document.createElement('h2');heading.textContent='F407 主控';
  const online=document.createElement('span');online.id='f407-online';online.className='tag';title.append(heading,online);
  const runtime=document.createElement('p');runtime.id='f407-runtime';runtime.className='muted';const counts=document.createElement('p');counts.id='f407-counts';
  const inertial=document.createElement('p');inertial.id='f407-inertial';inertial.className='muted';inertial.style.whiteSpace='pre-line';
  const toolbar=document.createElement('div');toolbar.className='toolbar';const output=document.createElement('pre');output.className='backend-json';output.hidden=true;
  for(const [label,operation,args]of [['读取两组 IMU','mcu-rpc',{command:'sensors'}],['读取惯导','mcu-rpc',{command:'ins.get'}],['停止主控输出','mcu-rpc',{command:'stop'}],['ST-Link 软件复位','target-reset',{target:'f407'}]]){
    const button=document.createElement('button');button.className='secondary';button.textContent=label;
    button.onclick=async()=>{button.disabled=true;try{const reply=await api(operation,args,'POST');if(!reply.ok)throw new Error(reply.error||reply.code);output.hidden=false;output.textContent=JSON.stringify(operation==='mcu-rpc'&&args.command==='sensors'?{mpu6050:reply.mpu6050,bmi088:reply.bmi088}:reply,null,2);await refreshStatus();}catch(error){notice(error.message,true);}finally{button.disabled=false;}};
    toolbar.append(button);
  }
  card.append(title,runtime,counts,inertial,toolbar,output);$('page-status').append(card);renderF407(state?.f407);
  $('f407-param-read').onclick=()=>readF407Parameters().catch(error=>notice(error.message,true));
  $('f407-param-key').onchange=()=>{$('f407-param-value').value=f407Parameters?.[$('f407-param-key').value]??'';};
  $('f407-param-form').onsubmit=async event=>{
    event.preventDefault();const key=$('f407-param-key').value,value=Number($('f407-param-value').value);
    try{if(!key||!Number.isInteger(value))throw new Error('请选择参数并填写整数');const reply=await api('param-set',{key,value},'POST');if(!reply.ok)throw new Error(reply.error||reply.code);await readF407Parameters();if(f407Parameters[key]!==value)throw new Error('写入后读回值不一致');notice('已写入主控 RAM，并完成读回确认。');}catch(error){notice(error.message,true);}
  };
  for(const [id,operation,message]of [['f407-param-save','param-save','主控已确认保存到 Flash。'],['f407-param-rollback','param-rollback','已恢复上次保存在主控 Flash 的参数。']]){
    $(id).onclick=async()=>{try{const reply=await api(operation,{},'POST');if(!reply.ok)throw new Error(reply.error||reply.code);await readF407Parameters();notice(message);}catch(error){notice(error.message,true);}};
  }
  document.querySelector('[data-page="profiles"]').addEventListener('click',()=>readF407Parameters().catch(error=>notice(error.message,true)));
  if(location.hash==='#profiles')readF407Parameters().catch(error=>notice(error.message,true));
});
