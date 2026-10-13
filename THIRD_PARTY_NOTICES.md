# Third-party notices

EDAA itself is released under the MIT License (`LICENSE`). It includes or is built with the following third-party components. Their
license texts are installed in `Program Files\EDAA\licenses` and are in the
`third_party/` submodules of the source repository.

## NVIDIA DLSS SDK (DLSS 310.9.1)

- Used for: NVIDIA DLAA (`nvsdk_ngx_s.lib` linked into `EDAA.addon64`,
  `nvngx_dlss.dll` shipped next to it).
- License: NVIDIA RTX SDKs License (`licenses/NVIDIA-RTX-SDKs-LICENSE.txt`).
- Source: https://github.com/NVIDIA/DLSS
- NVIDIA and DLSS are trademarks and/or registered trademarks of NVIDIA
  Corporation in the U.S. and other countries. EDAA is not sponsored or
  endorsed by NVIDIA.

The DLSS library includes third-party code that must be acknowledged. The
notices below are those of section 9.5 ("Notices") and 9.6 ("3rd Party
Software": CURL, 8x13 bitmap font, d3dx12.h, pugixml, libnpy, stb,
DirectX-Graphics-Samples, Vulkan-Headers) of the DLSS Programming Guide
(`third_party/dlss/doc/DLSS_Programming_Guide_Release.pdf`), reproduced as
required by section 4.2.3 of that guide.

### Notices

The information provided in this specification is believed to be accurate and reliable as of the date
provided. However, NVIDIA Corporation (“NVIDIA”) does not give any representations or warranties,
expressed or implied, as to the accuracy or completeness of such information. NVIDIA shall have no
liability for the consequences or use of such information or for any infringement of patents or other
rights of third parties that may result from its use. This publication supersedes and replaces all other
specifications for the product that may have been previously supplied.

NVIDIA reserves the right to make corrections, modifications, enhancements, improvements, and other
changes to this specification, at any time and/or to discontinue any product or service without notice.
Customer should obtain the latest relevant specification before placing orders and should verify that
such information is current and complete.

NVIDIA products are sold subject to the NVIDIA standard terms and conditions of sale supplied at the
time of order acknowledgement, unless otherwise agreed in an individual sales agreement signed by
authorized representatives of NVIDIA and customer. NVIDIA hereby expressly objects to applying any
customer general terms and conditions with regard to the purchase of the NVIDIA product referenced in
this specification.

NVIDIA products are not designed, authorized or warranted to be suitable for use in medical, military,
aircraft, space or life support equipment, nor in applications where failure or malfunction of the NVIDIA
product can reasonably be expected to result in personal injury, death or property or environmental
damage. NVIDIA accepts no liability for inclusion and/or use of NVIDIA products in such equipment or
applications and therefore such inclusion and/or use is at customer’s own risk.

NVIDIA makes no representation or warranty that products based on these specifications will be suitable
for any specified use without further testing or modification. Testing of all parameters of each product 
is not necessarily performed by NVIDIA. It is customer’s sole responsibility to ensure the product is
suitable and fit for the application planned by customer and to do the necessary testing for the
application in order to avoid a default of the application or the product. Weaknesses in customer’s
product designs may affect the quality and reliability of the NVIDIA product and may result in additional
or different conditions and/or requirements beyond those contained in this specification. NVIDIA does
not accept any liability related to any default, damage, costs or problem which may be based on or
attributable to: (i) the use of the NVIDIA product in any manner that is contrary to this specification, 
or (ii) customer product designs. 

No license, either expressed or implied, is granted under any NVIDIA patent right, copyright, or other
NVIDIA intellectual property right under this specification. Information published by NVIDIA regarding
third-party products or services does not constitute a license from NVIDIA to use such products or
services or a warranty or endorsement thereof. Use of such information may require a license from a
third party under the patents or other intellectual property rights of the third party, or a license from
NVIDIA under the patents or other intellectual property rights of NVIDIA. Reproduction of information in
this specification is permissible only if reproduction is approved by NVIDIA in writing, is reproduced
without alteration, and is accompanied by all associated conditions, limitations, and notices.

ALL NVIDIA DESIGN SPECIFICATIONS, REFERENCE BOARDS, FILES, DRAWINGS, DIAGNOSTICS, LISTS, AND
OTHER DOCUMENTS (TOGETHER AND SEPARATELY, “MATERIALS”) ARE BEING PROVIDED “AS IS.” NVIDIA
MAKES NO WARRANTIES, EXPRESSED, IMPLIED, STATUTORY, OR OTHERWISE WITH RESPECT TO THE
MATERIALS, AND EXPRESSLY DISCLAIMS ALL IMPLIED WARRANTIES OF NONINFRINGEMENT,
MERCHANTABILITY, AND FITNESS FOR A PARTICULAR PURPOSE.

Notwithstanding any damages that customer might incur for any reason whatsoever, NVIDIA’s
aggregate and cumulative liability towards customer for the products described herein shall be limited
in accordance with the NVIDIA terms and conditions of sale for the product.

#### Trademarks
NVIDIA and the NVIDIA logo are trademarks and/or registered trademarks of NVIDIA Corporation in the
U.S. and other countries. Other company and product names may be trademarks of the respective
companies with which they are associated.

#### Copyright
© 2018-2020 NVIDIA Corporation. All rights reserved.
www.nvidia.com

### 3rd Party Software

#### CURL
COPYRIGHT AND PERMISSION NOTICE

Copyright © 1996 - 2018, Daniel Stenberg, daniel@haxx.se, and many contributors, see the THANKS file.
All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose with or without fee is
hereby granted, provided that the above copyright notice and this permission notice appear in all
copies.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall not be used in advertising or
otherwise to promote the sale, use or other dealings in this Software without prior written authorization
of the copyright holder.

#### 8x13 BITMAP FONT

https://courses.cs.washington.edu/courses/cse457/98a/tech/OpenGL/font.c

```
* (c) Copyright 1993, Silicon Graphics, Inc.
* ALL RIGHTS RESERVED
* Permission to use, copy, modify, and distribute this software for
* any purpose and without fee is hereby granted, provided that the above
* copyright notice appear in all copies and that both the copyright notice
* and this permission notice appear in supporting documentation, and that
* the name of Silicon Graphics, Inc. not be used in advertising
* or publicity pertaining to distribution of the software without specific,
* written prior permission.
*
* THE MATERIAL EMBODIED ON THIS SOFTWARE IS PROVIDED TO YOU "AS-IS"
* AND WITHOUT WARRANTY OF ANY KIND, EXPRESS, IMPLIED OR OTHERWISE,
* INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY OR
* FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL SILICON
* GRAPHICS, INC. BE LIABLE TO YOU OR ANYONE ELSE FOR ANY DIRECT,
* SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY
* KIND, OR ANY DAMAGES WHATSOEVER, INCLUDING WITHOUT LIMITATION,
* LOSS OF PROFIT, LOSS OF USE, SAVINGS OR REVENUE, OR THE CLAIMS OF
* THIRD PARTIES, WHETHER OR NOT SILICON GRAPHICS, INC. HAS BEEN
* ADVISED OF THE POSSIBILITY OF SUCH LOSS, HOWEVER CAUSED AND ON
* ANY THEORY OF LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE
* POSSESSION, USE OR PERFORMANCE OF THIS SOFTWARE.
```

#### d3dx12.h

https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Multithreading/src/d3dx12.h

```
* Copyright © 2017 NVIDIA Corporation. All rights reserved.
*
* NOTICE TO USER:
*
* This software is subject to NVIDIA ownership rights under U.S. and international Copyright laws.
*
* This software and the information contained herein are PROPRIETARY and CONFIDENTIAL to NVIDIA
* and are being provided solely under the terms and conditions of an NVIDIA software license agreement
* and / or non-disclosure agreement. Otherwise, you have no rights to use or access this software in any manner.
*
* If not covered by the applicable NVIDIA software license agreement:
* NVIDIA MAKES NO REPRESENTATION ABOUT THE SUITABILITY OF THIS SOFTWARE FOR ANY PURPOSE.
* IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.
* NVIDIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
* INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
* IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
* OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
* NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOURCE CODE.
*
* U.S. Government End Users.
* This software is a "commercial item" as that term is defined at 48 C.F.R. 2.101 (OCT 1995),
* consisting of "commercial computer software" and "commercial computer software documentation"
* as such terms are used in 48 C.F.R. 12.212 (SEPT 1995) and is provided to the U.S. Government only as a commercial
end item.
* Consistent with 48 C.F.R.12.212 and 48 C.F.R. 227.7202-1 through 227.7202-4 (JUNE 1995),
* all U.S. Government End Users acquire the software with only those rights set forth herein.
*
* Any use of this software in individual and commercial software must include,
* in the user documentation and internal comments to the code,
* the above Disclaimer (as applicable) and U.S. Government End Users Notice.
*
* Original code by Microsoft Corporation was obtained from
* https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Multithreading
* and licensed under MIT license (below)
* NVIDIA license applies to changes made relative to original code.
* Copyright (c) Microsoft. All rights reserved.
* This code is licensed under the MIT License (MIT).
* THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
* IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
```

#### xml

https://pugixml.org/license.html

```
* This library is available to anybody free of charge, under the terms of MIT License:
* Copyright (c) 2006-2018 Arseny Kapoulkine
* Permission is hereby granted, free of charge, to any person obtaining a copy of this
* software and associated documentation files (the “Software”), to deal in the Software
* without restriction, including without limitation the rights to use, copy, modify, merge,
* publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
* to whom the Software is furnished to do so, subject to the following conditions:
* The above copyright notice and this permission notice shall be included in all copies or
* substantial portions of the Software.
* THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
* PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
* FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
* OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
* This means that you can freely use pugixml in your applications, both open-source and
* proprietary. If you use pugixml in a product, it is sufficient to add an acknowledgment
* like this to the product distribution:
* This software is based on pugixml library (http://pugixml.org). pugixml is Copyright (C) *
2006-2018 Arseny Kapoulkine.

```

#### npy

https://github.com/llohse/libnpy

https://github.com/llohse/libnpy/blob/master/LICENSE

```
* MIT License
* Copyright (c) 2021 Leon Merten Lohse
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
```

#### stb

```
/*
* ALTERNATIVE A - MIT License
* Copyright (c) 2017 Sean Barrett
* Permission is hereby granted, free of charge, to any person obtaining a copy of
* this software and associated documentation files (the "Software"), to deal in
* the Software without restriction, including without limitation the rights to
* use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
* of the Software, and to permit persons to whom the Software is furnished to do
* so, subject to the following conditions:
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
```

#### DirectX-Graphics-Samples

https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Multithreading

```
/*
* Copyright (c) Microsoft. All rights reserved.
* This code is licensed under the MIT License (MIT).
* THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
* IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
* PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
*/
```

#### Vulkan-Headers

https://github.com/KhronosGroup/Vulkan-Headers/

```
Copyright 2015-2023 The Khronos Group Inc.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

## ReShade (add-on API headers, v6.8.0)

- License: BSD 3-Clause (`licenses/ReShade-LICENSE.md`).
- Source: https://github.com/crosire/reshade

## Dear ImGui (headers)

- License: MIT (`licenses/ImGui-LICENSE.txt`).
- Source: https://github.com/ocornut/imgui
