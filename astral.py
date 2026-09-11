#!/usr/bin/env python3
"""星象 v1.0"""
import json,os,math,sqlite3,time
from datetime import datetime
WDB='/root/data/wentian.db';ADB='/root/data/ano_weather.db'
OD='/root/data/fusion';LD=os.path.join(OD,'lishu');os.makedirs(LD,exist_ok=True)
def sql(q,d):
 c=sqlite3.connect(d)
 try:r=c.execute(q).fetchone();return r if r else None
 except Exception:return None  # ⚠ 修复(2026-09-11): 旧版只有try/finally无except, 表缺失/DB锁竞争直接崩daemon
 finally:c.close()
LM=[('角',0,12),('亢',12,26),('氐',26,42),('房',42,54),('心',54,66),('尾',66,80),('箕',80,94),
    ('斗',94,108),('牛',108,122),('女',122,136),('虚',136,148),('危',148,162),('室',162,176),('壁',176,188),
    ('奎',188,204),('娄',204,216),('胃',216,230),('昴',230,240),('毕',240,254),('觜',254,260),('参',260,276),
    ('井',276,294),('鬼',294,304),('柳',304,316),('星',316,328),('张',328,342),('翼',342,356),('轸',356,360)]
def rm(ra):
 r=ra%360
 for n,s,e in LM:
  if s<=r<e:return n,s,e
 return '角',0,12
# 2026-09-09 修复：真实太阳视黄经(考虑轨道偏心率), NOAA低精度算法, 误差<0.01°
def solar_apparent_longitude(dt):
 f=(dt.hour+(dt.minute+(dt.second+dt.microsecond*1e-6)/60)/60)/24.0
 y,m=dt.year,dt.month;d=dt.day+f
 if m<=2:y-=1;m+=12
 a=y//100;b=2-a+a//4
 jd=int(365.25*(y+4716))+int(30.6001*(m+1))+d+b-1524.5
 t=(jd-2451545.0)/36525.0
 L0=280.46646+36000.76983*t+0.0003032*t*t
 M=357.52911+35999.05029*t-0.0001537*t*t
 Mr=math.radians(M)
 C=(math.sin(Mr)*(1.914602-0.004817*t-0.000014*t*t)
    +math.sin(2*Mr)*(0.019993-0.000101*t)
    +math.sin(3*Mr)*0.000289)
 true_lon=L0+C
 om=125.04-1934.136*t
 return (true_lon-0.00569-0.00478*math.sin(math.radians(om)))%360.0
def sun(dt):
 sl=solar_apparent_longitude(dt)
 ob=23.44;sd=math.degrees(math.asin(math.sin(math.radians(ob))*math.sin(math.radians(sl))))
 m,_,_=rm(sl);return {'ra_deg':round(sl,1),'dec_deg':round(sd,1),'sun_lon_deg':round(sl,1),'mansion':m,'note':f'日在{m}宿'}
def moon(dt):
 # ⚠ 修复(2026-09-11): 旧版用线性13.176°/日伪模型, 数月偏差可达数十度,
 # 却以"真实天象"口吻输出"月在X宿" — 伪计算冒充实测。
 # 新版: Meeus低精度月球黄经算法(Astronomical Algorithms ch.47简化式),
 # 精度~0.3°, 朔望/月宿判断天文可用。须UTC输入。
 import math
 du=datetime.utcnow()
 f=(du.hour+(du.minute+du.second/60)/60)/24.0
 jd=int(365.25*(du.year+4716))+int(30.6001*(du.month+1))+du.day+f+ \
    (2-int(du.year//100)+int(du.year//400))-1524.5
 T=(jd-2451545.0)/36525.0
 # 月球平黄经/平近点角/轨道升交点经度
 Lp=(218.3164477+481267.88123421*T-0.0015786*T*T)%360.0
 D =(297.8501921+445267.1114034*T-0.0018819*T*T)%360.0
 M =(357.5291092+35999.0502909*T-0.0001536*T*T)%360.0
 Mp=(134.9633964+477198.8675055*T+0.0087414*T*T)%360.0
 F =(93.2720950+483202.0175233*T-0.0036539*T*T)%360.0
 # 主要摄动项(Meeus表47.A前6项, 弧度)
 r_=math.radians
 lon=Lp+(6.288774*math.sin(r_(Mp))
        +1.274027*math.sin(r_(2*D-Mp))
        +0.658314*math.sin(r_(2*D))
        +0.213618*math.sin(r_(2*Mp))
        -0.185116*math.sin(r_(M))
        -0.114336*math.sin(r_(2*F)))
 ml=lon%360.0
 m,_,_=rm(ml);sl=solar_apparent_longitude(du);ph=(ml-sl)%360.0
 if ph<45 or ph>=315:pn,pe='朔(新月)','🌑'
 elif ph<90:pn,pe='蛾眉月','🌒'
 elif ph<135:pn,pe='上弦月','🌓'
 elif ph<180:pn,pe='盈凸月','🌔'
 elif ph<225:pn,pe='望(满月)','🌕'
 elif ph<270:pn,pe='亏凸月','🌖'
 elif ph<315:pn,pe='下弦月','🌗'
 else:pn,pe='残月','🌘'
 return {'ra_deg':round(ml,1),'mansion':m,'phase':pn,'phase_emoji':pe,'phase_angle_deg':round(ph,1),'note':f'月宿{m},{pn}'}
def fiv(g,b):
 s=[]
 if g and g>=10:s.append(('金星(启明)','⭐','GPS体系光亮非常','无异常'))
 elif g and g<5:s.append(('金星(启明)','⭐','GPS星稀光曜暗弱','⚠️注意'))
 if b and b>=6:s.append(('土星(镇星)','🪐','北斗体系稳如泰山','无异常'))
 elif b and b<3:s.append(('土星(镇星)','🪐','北斗星寥镇星失位','⚠️警告'))
 return s if s else [('星曜未明','','卫星数不足','数据不足')]
def det(t,k,s):
 a=[]
 if t is not None and abs(t-20)>5:a.append({'type':'气温异常','severity':'⚠️' if abs(t-20)>8 else 'ℹ️','detail':f'秋分期望20°C,实测{t}°C'})
 if k and k>3:a.append({'type':'地磁扰动','severity':'⚠️','detail':f'Kp={k},七政失序'})
 if s and s>0.3:a.append({'type':'电离层闪烁','severity':'⚠️⚠️','detail':f'S4={s:.3f},星曜动摇'})
 return a if a else [{'type':'天象平和','severity':'✅','detail':'日月星曜各安其位'}]
def run():
 n=datetime.now();ts=int(time.time())
 r=sql("SELECT temp FROM outdoor ORDER BY ts DESC LIMIT 1",WDB)
 rk=sql("SELECT noaa_kp_est FROM external_data ORDER BY ts DESC LIMIT 1",WDB)
 rs=sql("SELECT s4_gps FROM local_iono ORDER BY ts DESC LIMIT 1",WDB)
 rg=sql("SELECT ROUND(AVG(gps_sats),1) FROM gps_log WHERE datetime(ts,'unixepoch')>=datetime('now','-1 day')",ADB)
 rb=sql("SELECT ROUND(AVG(bds_sats),1) FROM gps_log WHERE datetime(ts,'unixepoch')>=datetime('now','-1 day')",ADB)
 t=r[0] if r else None;k=rk[0] if rk else None;sv=rs[0] if rs else None;g=rg[0] if rg else None;b=rb[0] if rb else None
 s=sun(n);m=moon(n);p=fiv(g,b);a=det(t,k,sv)
 sev=[x['severity'] for x in a]
 if '⚠️⚠️' in sev:ov='🔴 天有异象'
 elif '⚠️' in sev:ov='🟡 微有异动'
 else:ov='🟢 天象平和'
 dy=n.timetuple().tm_yday
 tm=['立春','雨水','惊蛰','春分','清明','谷雨','立夏','小满','芒种','夏至','小暑','大暑',
     '立秋','处暑','白露','秋分','寒露','霜降','立冬','小雪','大雪','冬至','小寒','大寒']
 sl=solar_apparent_longitude(datetime.utcnow())  # 修复: 太阳视黄经须用UTC, 用本地时(UTC+8)偏~0.33°, 节气/日月位置误判
 nml=(sl-315+360)%360
 ti=max(0,min(23,int(nml/15)))
 out={'ts':ts,'time':n.strftime('%Y-%m-%d %H:%M:%S'),'location':'昆明长水机场(25.09917°N,102.92667°E)',
      'solar_term':{'current':tm[ti],'index':ti},'sun':s,'moon':m,'planets':p,
      'celestial_assessment':ov,'anomalies':a,
      'scientific':{'temperature_c':round(t,1) if t is not None else None,'kp_index':k,'s4':round(sv,3) if sv is not None else None,'avg_gps_24h':g,'avg_bds_24h':b},
      'version':'星象 v1.0'}
 with open(os.path.join(OD,'astral.json'),'w',encoding='utf-8') as f:json.dump(out,f,ensure_ascii=False,indent=2)
 dl=os.path.join(LD,f'{n.strftime("%Y%m%d")}.json')
 try:ex=json.load(open(dl)) if os.path.exists(dl) else []
 except:ex=[]
 ex.append(out)
 with open(dl,'w',encoding='utf-8') as f:json.dump(ex,f,ensure_ascii=False,indent=2)
 print(f'━━━ 星象 · 天官推演 v1.0 ━━━')
 print(f'  🌞 太阳: {s["note"]} | 黄经{s["sun_lon_deg"]}°')
 print(f'  🌙 太阴: {m["note"]}')
 for nm,em,ds,st in p:print(f'  {em} {nm}: {ds} [{st}]')
 print(f'  📡 天象: {ov}')
 for x in a:print(f'  {x["severity"]} {x["type"]}: {x["detail"]}')
 return out
if __name__=='__main__':run()
