'use strict';
(function(root){
  function render(bytes,start,marks,label,previousByte=-1,showTimestamps=true){
    const decoder=new TextDecoder('utf-8');let output='',position=0,markIndex=0;
    if(!showTimestamps)return decoder.decode(bytes).replace(/\0/g,'␀');
    // A page can start between CR and LF. The previous page already ended the line.
    if(start>0&&previousByte===13&&bytes[0]===10)position=1;
    let continuation=start>0&&previousByte!==10&&previousByte!==13;
    while(position<bytes.length){
      const absolute=start+position;let end=position;
      while(end<bytes.length&&bytes[end]!==10&&bytes[end]!==13)end++;
      while(markIndex<marks.length&&marks[markIndex].offset+marks[markIndex].count<=absolute)markIndex++;
      const mark=marks[markIndex],stamp=mark&&mark.offset<=absolute&&absolute<mark.offset+mark.count?mark:null;
      const title=continuation?'续行':stamp?label(stamp.utc,stamp.uptime):'时间戳缺失';
      output+=`[${title} · @${absolute}] ${decoder.decode(bytes.subarray(position,end)).replace(/\0/g,'␀')}`;
      if(end<bytes.length){
        output+='\n';position=end+1;
        if(bytes[end]===13&&bytes[position]===10)position++;
      }else position=end;
      continuation=false;
    }
    return output;
  }
  const api={render};
  if(typeof module==='object'&&module.exports)module.exports=api;
  else root.GatewayLogText=api;
})(globalThis);
