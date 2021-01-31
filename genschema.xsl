<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" 
	xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
	<xsl:output method="xml" indent="yes" encoding="utf-8" />

	<xsl:template match="/*">
		<schemalist>
			<xsl:apply-templates select="plugin|object"/>
		</schemalist>
	</xsl:template>

	<xsl:template match="plugin">
		<schema id="org.wayfire.section.{@name}"
			path="/org/wayfire/section/{@name}/">
			<xsl:apply-templates select="option|group"/>
		</schema>
	</xsl:template>

	<xsl:template match="object">
		<schema id="org.wayfire.section.{@name}">
			<xsl:apply-templates select="option|group"/>
		</schema>
	</xsl:template>

	<xsl:template match="group">
		<xsl:apply-templates select="option|subgroup"/>
	</xsl:template>

	<xsl:template match="subgroup">
		<xsl:apply-templates select="option"/>
	</xsl:template>

	<xsl:template match="option">
		<key name="{translate(@name, '_', '-')}">
			<xsl:attribute name="type">
				<xsl:choose>
					<xsl:when test="@type = 'int'">i</xsl:when>
					<xsl:when test="@type = 'bool'">b</xsl:when>
					<xsl:when test="@type = 'double'">d</xsl:when>
					<xsl:when test="@type = 'color'">(dddd)</xsl:when>
					<xsl:when test="@type = 'key' or @type = 'button' or @type = 'gesture' or @type = 'activator' or @type = 'output::mode' or @type = 'output::position' or @type = 'string'">s</xsl:when>
					<xsl:when test="@type = 'dynamic_list'">s</xsl:when> <!-- old unsupported -->
					<xsl:when test="@type = 'dynamic-list'">a{s(<xsl:for-each select="entry"><xsl:choose>
							<xsl:when test="@type = 'int'">i</xsl:when>
							<xsl:when test="@type = 'bool'">b</xsl:when>
							<xsl:when test="@type = 'double'">d</xsl:when>
							<xsl:when test="@type = 'color'">(dddd)</xsl:when>
							<xsl:when test="@type = 'key' or @type = 'button' or @type = 'gesture' or @type = 'activator' or @type = 'output::mode' or @type = 'output::position' or @type = 'string'">s</xsl:when>
						</xsl:choose></xsl:for-each>)}</xsl:when>
				</xsl:choose>
			</xsl:attribute>
			<default>
				<xsl:choose>
					<xsl:when test="default and (@type = 'key' or @type = 'button' or @type = 'string')">'<xsl:value-of select="default"/>'</xsl:when>
					<!-- XXX: hex color not supported yet // maybe: https://stackoverflow.com/a/22911132 -->
					<xsl:when test="default and @type = 'color' and substring(default, 1, 1) = '#'">(0.0,0.0,0.0,0.0)</xsl:when>
					<xsl:when test="default and @type = 'color'">(<xsl:value-of select="translate(default,' ',',')"/>)</xsl:when>
					<xsl:when test="default = 0 and @type = 'bool'">false</xsl:when>
					<xsl:when test="default = 1 and @type = 'bool'">true</xsl:when>
					<!-- TODO: make bool values lowercase -->
					<xsl:when test="default and (@type = 'key' or @type = 'button' or @type = 'gesture' or @type = 'activator' or @type = 'output::mode' or @type = 'output::position' or @type = 'string')">'<xsl:value-of select="default"/>'</xsl:when>
					<xsl:when test="default"><xsl:value-of select="default"/></xsl:when>
					<xsl:when test="@type = 'int'">0</xsl:when>
					<xsl:when test="@type = 'bool'">false</xsl:when>
					<xsl:when test="@type = 'double'">0.0</xsl:when>
					<xsl:when test="@type = 'color'">(0.0,0.0,0.0,0.0)</xsl:when>
					<xsl:when test="@type = 'key' or @type = 'button' or @type = 'gesture' or @type = 'activator' or @type = 'string'">''</xsl:when>
					<xsl:when test="@type = 'dynamic_list'">''</xsl:when> <!-- old unsupported -->
					<xsl:when test="@type = 'dynamic-list'">{}</xsl:when>
				</xsl:choose>
			</default>
			<summary><xsl:value-of select="_short"/></summary>
			<xsl:choose>
				<xsl:when test="min and max"><range min="{min}" max="{max}"/></xsl:when>
				<xsl:when test="min"><range min="{min}"/></xsl:when>
				<xsl:when test="max"><range max="{max}"/></xsl:when>
			</xsl:choose>
			<xsl:if test="desc">
				<choices>
					<xsl:for-each select="desc">
						<choice value="{value}"/>
					</xsl:for-each>
				</choices>
			</xsl:if>
		</key>
	</xsl:template>

</xsl:stylesheet>
